/*
 * sofs.c - Implementação (esqueleto) do sistema de arquivos sofs.
 *
 * A camada de blocos (sofs-block) é usada para todos os acessos ao disco;
 * a camada de bitmap (bitmap2) gerencia o controle de blocos e i-nodes livres.
 *
 * Layout do sistema de arquivos dentro de uma partição (em ordem):
 *   [bloco 0]          superbloco
 *   [blocos 1 .. bb]   bitmap de blocos livres   (bb = freeBlocksBitmapSize)
 *   [bb+1 .. bb+bi]    bitmap de i-nodes livres  (bi = freeInodeBitmapSize)
 *   [bb+bi+1 .. ...]   área de i-nodes           (10% dos blocos, arredondado para cima)
 *   [resto]            blocos de dados
 *
 * As funções marcadas com TODO são responsabilidade do grupo.
 * As funções auxiliares alloc_data_block(), free_data_block(),
 * alloc_inode() e free_inode() são fornecidas como blocos de construção.
 */

#include <string.h>
#include "sofs.h"
#include "sofs-block.h"

/* -------------------------------------------------------------------------
 * Estado interno de montagem
 * ---------------------------------------------------------------------- */

static int g_mounted = false;
static struct sofs_superbloco g_superbloco;
static unsigned int g_superbloco_sector;   /* setor absoluto do superbloco */

#define SOFS_OPEN_FILE_MAX 16
#define MAX_LINK_DEPTH     8

struct sofs_open_file {
    int in_use;
    unsigned int inodeNumber;
    DWORD position;
};

static struct sofs_open_file g_open_files[SOFS_OPEN_FILE_MAX];

/* -------------------------------------------------------------------------
 * Auxiliar: lê o MBR e localiza a partição <partition>.
 * Preenche *first_sector e *num_sectors.
 * Retorna 0 em caso de sucesso.
 * ---------------------------------------------------------------------- */
static int read_partition_info(int partition,
                               unsigned int *first_sector,
                               unsigned int *num_sectors)
{
    unsigned char mbr_buf[SECTOR_SIZE];
    struct sofs_mbr *mbr;

    if (read_sector(0, mbr_buf) != 0)
        return -1;

    mbr = (struct sofs_mbr *)mbr_buf;

    if (partition < 0 || partition >= (int)mbr->numPartitions)
        return -1;

    *first_sector = mbr->partitionTable[partition].firstSector;
    *num_sectors  = mbr->partitionTable[partition].lastSector
                    - mbr->partitionTable[partition].firstSector + 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Funções básicas de criação/destruição de blocos de dados e i-nodes.
 *
 * Fornecidas como blocos de construção para a implementação do grupo em
 * sofs_create, sofs_delete, sofs_read, sofs_write, etc.
 * ---------------------------------------------------------------------- */

/*
 * alloc_data_block - aloca o primeiro bloco de dados livre.
 *
 * Pesquisa no bitmap de dados o primeiro bit livre, marca-o como ocupado,
 * zera o conteúdo do bloco e retorna o número absoluto do bloco na partição.
 *
 * Retorna o número do bloco (>= 0) em caso de sucesso; -1 em caso de erro
 * ou se o disco estiver cheio.
 */
static int alloc_data_block(void)
{
    int bit;
    unsigned int block_size;
    unsigned char *buf;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_DADOS, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_DADOS, bit, 1) != 0)
        return -1;

    /* Inicializa o bloco recém-alocado com zeros */
    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    buf = (unsigned char *)__builtin_alloca(block_size);
    memset(buf, 0, block_size);

    /* O primeiro bloco de dados começa após superbloco + bitmaps + área de i-nodes */
    unsigned int first_data_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + g_superbloco.inodeAreaSize;

    if (write_block(first_data_block + (unsigned int)bit, buf) != 0) {
        setBitmap2(BITMAP_DADOS, bit, 0);
        return -1;
    }

    return (int)(first_data_block + (unsigned int)bit);
}

/*
 * free_data_block - libera um bloco de dados previamente alocado.
 *
 *   abs_block_num : número absoluto do bloco na partição (conforme
 *                   retornado por alloc_data_block).
 *
 * Retorna 0 em caso de sucesso; -1 em caso de erro.
 */
static int free_data_block(unsigned int abs_block_num)
{
    unsigned int first_data_block;
    int bit;

    if (!g_mounted)
        return -1;

    first_data_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + g_superbloco.inodeAreaSize;

    if (abs_block_num < first_data_block)
        return -1;

    bit = (int)(abs_block_num - first_data_block);
    return setBitmap2(BITMAP_DADOS, bit, 0);
}

/*
 * alloc_inode - aloca o primeiro i-node livre.
 *
 * Pesquisa no bitmap de i-nodes o primeiro bit livre, marca-o como ocupado,
 * zera o conteúdo do i-node em disco e retorna o número do i-node.
 *
 * Retorna o número do i-node (>= 0) em caso de sucesso; -1 em caso de erro
 * ou se todos os i-nodes estiverem em uso.
 */
static int alloc_inode(void)
{
    int bit;
    unsigned int inode_block;
    unsigned int inodes_per_block;
    unsigned int inode_offset;
    unsigned char *buf;
    unsigned int block_size;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_INODE, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_INODE, bit, 1) != 0)
        return -1;

    /* Zera o i-node em disco */
    block_size     = g_superbloco.blockSize * SECTOR_SIZE;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    inode_block    = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + (unsigned int)bit / inodes_per_block;
    inode_offset   = (unsigned int)bit % inodes_per_block;

    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0) {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    memset(buf + inode_offset * sizeof(struct sofs_inode), 0,
           sizeof(struct sofs_inode));

    {
        struct sofs_inode *inode =
            (struct sofs_inode *)(buf + inode_offset * sizeof(struct sofs_inode));
        inode->RefCounter = 1;
    }

    if (write_block(inode_block, buf) != 0) {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    return bit;
}

/*
 * free_inode - libera um i-node previamente alocado.
 *
 *   inode_num : número do i-node (conforme retornado por alloc_inode).
 *
 * Retorna 0 em caso de sucesso; -1 em caso de erro.
 */
static int free_inode(unsigned int inode_num)
{
    if (!g_mounted)
        return -1;

    return setBitmap2(BITMAP_INODE, (int)inode_num, 0);
}

static unsigned int get_root_dir_block(void)
{
    return 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + g_superbloco.inodeAreaSize;
}

static int find_directory_entry(const char *name, unsigned int *inode_num, int *index)
{
    unsigned int block_size;
    unsigned int entries_per_block;
    unsigned char *buf;
    struct sofs_record *records;
    unsigned int i;

    if (!g_mounted || name == NULL || inode_num == NULL || index == NULL)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    entries_per_block = block_size / sizeof(struct sofs_record);
    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(get_root_dir_block(), buf) != 0)
        return -1;

    records = (struct sofs_record *)buf;
    for (i = 0; i < entries_per_block; i++) {
        if (records[i].TypeVal != TYPEVAL_INVALIDO
            && strcmp(records[i].name, name) == 0) {
            *inode_num = records[i].inodeNumber;
            *index = (int)i;
            return 0;
        }
    }

    return -1;
}

static int set_directory_entry(const char *name, unsigned int inode_num,
                               BYTE file_type, int *index)
{
    unsigned int block_size;
    unsigned int entries_per_block;
    unsigned char *buf;
    struct sofs_record *records;
    unsigned int i;

    if (!g_mounted || name == NULL)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    entries_per_block = block_size / sizeof(struct sofs_record);
    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(get_root_dir_block(), buf) != 0)
        return -1;

    records = (struct sofs_record *)buf;
    for (i = 0; i < entries_per_block; i++) {
        if (records[i].TypeVal != TYPEVAL_INVALIDO
            && strcmp(records[i].name, name) == 0) {
            records[i].TypeVal = file_type;
            records[i].inodeNumber = inode_num;
            if (index != NULL)
                *index = (int)i;
            return write_block(get_root_dir_block(), buf);
        }
    }

    for (i = 0; i < entries_per_block; i++) {
        if (records[i].TypeVal == TYPEVAL_INVALIDO) {
            memset(&records[i], 0, sizeof(records[i]));
            records[i].TypeVal = file_type;
            strncpy(records[i].name, name, SOFS_MAX_FILE_NAME_SIZE);
            records[i].name[SOFS_MAX_FILE_NAME_SIZE] = '\0';
            records[i].inodeNumber = inode_num;
            if (index != NULL)
                *index = (int)i;
            return write_block(get_root_dir_block(), buf);
        }
    }

    return -1;
}

static int truncate_inode(unsigned int inode_num)
{
    unsigned int block_size;
    unsigned int inodes_per_block;
    unsigned int inode_block;
    unsigned int inode_offset;
    unsigned char *buf;
    struct sofs_inode *inode;

    if (!g_mounted)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    inode_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + (inode_num / inodes_per_block);
    inode_offset = inode_num % inodes_per_block;

    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0)
        return -1;

    inode = (struct sofs_inode *)(buf + inode_offset * sizeof(struct sofs_inode));

    if (inode->dataPtr[0] != 0) {
        free_data_block(inode->dataPtr[0]);
        inode->dataPtr[0] = 0;
    }
    if (inode->dataPtr[1] != 0) {
        free_data_block(inode->dataPtr[1]);
        inode->dataPtr[1] = 0;
    }
    inode->singleIndPtr = 0;
    inode->doubleIndPtr = 0;
    inode->blocksFileSize = 0;
    inode->bytesFileSize = 0;
    inode->RefCounter = 1;
    inode->reservado = 0;

    return write_block(inode_block, buf);
}

static int read_inode(unsigned int inode_num, struct sofs_inode *inode)
{
    unsigned int block_size;
    unsigned int inodes_per_block;
    unsigned int inode_block;
    unsigned int inode_offset;
    unsigned char *buf;

    if (!g_mounted || inode == NULL)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    inode_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + (inode_num / inodes_per_block);
    inode_offset = inode_num % inodes_per_block;

    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0)
        return -1;

    memcpy(inode, buf + inode_offset * sizeof(struct sofs_inode),
           sizeof(struct sofs_inode));
    return 0;
}

static int write_inode(unsigned int inode_num, const struct sofs_inode *inode)
{
    unsigned int block_size;
    unsigned int inodes_per_block;
    unsigned int inode_block;
    unsigned int inode_offset;
    unsigned char *buf;

    if (!g_mounted || inode == NULL)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    inode_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + (inode_num / inodes_per_block);
    inode_offset = inode_num % inodes_per_block;

    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0)
        return -1;

    memcpy(buf + inode_offset * sizeof(struct sofs_inode), inode,
           sizeof(struct sofs_inode));
    return write_block(inode_block, buf);
}

static int invalidate_directory_entry(const char *name)
{
    unsigned int block_size;
    unsigned int entries_per_block;
    unsigned char *buf;
    struct sofs_record *records;
    unsigned int i;

    if (!g_mounted || name == NULL)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    entries_per_block = block_size / sizeof(struct sofs_record);
    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(get_root_dir_block(), buf) != 0)
        return -1;

    records = (struct sofs_record *)buf;
    for (i = 0; i < entries_per_block; i++) {
        if (records[i].TypeVal != TYPEVAL_INVALIDO
            && strcmp(records[i].name, name) == 0) {
            memset(&records[i], 0, sizeof(records[i]));
            return write_block(get_root_dir_block(), buf);
        }
    }

    return -1;
}

static int find_free_handle(void)
{
    int i;

    for (i = 0; i < SOFS_OPEN_FILE_MAX; i++) {
        if (!g_open_files[i].in_use)
            return i + 1;
    }

    return -1;
}

/* -------------------------------------------------------------------------
 * Gerência do sistema de arquivos
 * ---------------------------------------------------------------------- */

int sofs_identify(char *name, int size)
{
    const char *id = "TODO implementation";
    if (name == NULL || size <= 0)
        return -1;
    strncpy(name, id, size - 1);
    name[size - 1] = '\0';
    return 0;
}

int sofs_format(int partition, int sectors_per_block)
{
    unsigned int first_sector, num_sectors;
    unsigned int num_blocks;
    unsigned int inode_area_blocks;
    unsigned int bitmap_blocks_data;
    unsigned int bitmap_blocks_inode;
    unsigned char block_buf[sectors_per_block * SECTOR_SIZE];
    struct sofs_superbloco *sb;
    unsigned int root_dir_block;

    if (sectors_per_block <= 0)
        return -1;

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    /* Inicializa a camada de blocos para poder escrever na partição */
    if (init_block_layer(first_sector, (unsigned int)sectors_per_block) != 0)
        return -1;

    num_blocks = num_sectors / (unsigned int)sectors_per_block;

    /* 10% dos blocos para i-nodes, arredondado para cima */
    inode_area_blocks = (num_blocks + 9) / 10;

    /* Um bloco por 8*(sectors_per_block*SECTOR_SIZE) bits necessários em cada bitmap */
    bitmap_blocks_data  = (num_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1)
                          / (8 * sectors_per_block * SECTOR_SIZE);
    bitmap_blocks_inode = (inode_area_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1)
                          / (8 * sectors_per_block * SECTOR_SIZE);

    /* Constrói e grava o superbloco (bloco 0 da partição) */
    memset(block_buf, 0, sizeof(block_buf));
    sb = (struct sofs_superbloco *)block_buf;
    memcpy(sb->id, "SOFS", 4);
    sb->version              = 0x7E32;
    sb->superblockSize       = 1;
    sb->freeBlocksBitmapSize = (WORD)bitmap_blocks_data;
    sb->freeInodeBitmapSize  = (WORD)bitmap_blocks_inode;
    sb->inodeAreaSize        = (WORD)inode_area_blocks;
    sb->blockSize            = (WORD)sectors_per_block;
    sb->diskSize             = (DWORD)num_blocks;

    /* Checksum: complemento de um da soma dos 5 primeiros DWORDs */
    {
        DWORD *words = (DWORD *)block_buf;
        DWORD  sum   = words[0] + words[1] + words[2] + words[3] + words[4];
        sb->Checksum = ~sum;
    }

    if (write_block(0, block_buf) != 0)
        return -1;

    /* Inicializa as áreas de bitmap e de i-nodes com zeros e reserva o bloco do diretório raiz */
    if (openBitmap2((int)first_sector) != 0)
        return -1;

    if (setBitmap2(BITMAP_DADOS, 0, 1) != 0) {
        closeBitmap2();
        return -1;
    }

    root_dir_block = 1
        + sb->freeBlocksBitmapSize
        + sb->freeInodeBitmapSize
        + sb->inodeAreaSize;

    memset(block_buf, 0, sizeof(block_buf));
    if (write_block(root_dir_block, block_buf) != 0) {
        closeBitmap2();
        return -1;
    }

    if (closeBitmap2() != 0)
        return -1;

    return 0;
}

int sofs_mount(int partition)
{
    unsigned int first_sector, num_sectors;
    unsigned char sector_buf[SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (g_mounted)
        return -1;  /* partição já montada */

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    /* Lê o primeiro setor da partição para obter o superbloco */
    if (read_sector(first_sector, sector_buf) != 0)
        return -1;

    sb = (struct sofs_superbloco *)sector_buf;

    /* Valida a assinatura do sistema de arquivos */
    if (memcmp(sb->id, "SOFS", 4) != 0)
        return -1;

    /* Agora sabemos o tamanho do bloco: inicializa a camada de blocos */
    if (init_block_layer(first_sector, (unsigned int)sb->blockSize) != 0)
        return -1;

    /* Abre o subsistema de bitmap */
    g_superbloco_sector = first_sector;
    if (openBitmap2((int)g_superbloco_sector) != 0)
        return -1;

    /* Armazena em cache o superbloco */
    memcpy(&g_superbloco, sb, sizeof(g_superbloco));
    g_mounted = true;
    return 0;
}

int sofs_umount(void)
{
    if (!g_mounted)
        return -1;

    closeBitmap2();
    reset_block_layer();
    memset(&g_superbloco, 0, sizeof(g_superbloco));
    g_mounted = false;
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de arquivo (TODO)
 * ---------------------------------------------------------------------- */

SOFS_FILE sofs_create(char *filename)
{
    unsigned int inode_num;
    int handle;
    int index;

    if (!g_mounted || filename == NULL || filename[0] == '\0')
        return -1;

    if (strlen(filename) > SOFS_MAX_FILE_NAME_SIZE)
        return -1;

    if (find_directory_entry(filename, &inode_num, &index) == 0) {
        if (truncate_inode(inode_num) != 0)
            return -1;
    } else {
        inode_num = (unsigned int)alloc_inode();
        if ((int)inode_num < 0)
            return -1;

        if (set_directory_entry(filename, inode_num, TYPEVAL_REGULAR, NULL) != 0)
            return -1;
    }

    handle = find_free_handle();
    if (handle < 0)
        return -1;

    g_open_files[handle - 1].in_use = 1;
    g_open_files[handle - 1].inodeNumber = inode_num;
    g_open_files[handle - 1].position = 0;

    return handle;
}

int sofs_delete(char *name)
{
    unsigned int inode_num;
    int index;
    struct sofs_inode inode;

    if (!g_mounted || name == NULL || name[0] == '\0')
        return -1;

    if (find_directory_entry(name, &inode_num, &index) != 0)
        return -1;

    if (read_inode(inode_num, &inode) != 0)
        return -1;

    if (inode.dataPtr[0] != 0)
        free_data_block(inode.dataPtr[0]);
    if (inode.dataPtr[1] != 0)
        free_data_block(inode.dataPtr[1]);

    if (free_inode(inode_num) != 0)
        return -1;

    if (invalidate_directory_entry(name) != 0)
        return -1;

    return 0;
}

SOFS_FILE sofs_open(char *name)
{
    unsigned int inode_num;
    int handle;
    int index;
    int depth;
    char curname[SOFS_MAX_FILE_NAME_SIZE + 2];
    unsigned int block_size;
    unsigned char *buf;
    unsigned char *buf2;
    struct sofs_record *records;
    struct sofs_inode inode;

    if (!g_mounted || name == NULL || name[0] == '\0')
        return -1;

    strncpy(curname, name, SOFS_MAX_FILE_NAME_SIZE);
    curname[SOFS_MAX_FILE_NAME_SIZE] = '\0';

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    buf  = (unsigned char *)__builtin_alloca(block_size);
    buf2 = (unsigned char *)__builtin_alloca(block_size);

    for (depth = 0; depth < MAX_LINK_DEPTH; depth++) {
        if (find_directory_entry(curname, &inode_num, &index) != 0)
            return -1;

        if (read_block(get_root_dir_block(), buf) != 0)
            return -1;

        records = (struct sofs_record *)buf;
        if (records[index].TypeVal != TYPEVAL_LINK)
            break;

        /* follow the soft link: read target name from data block */
        if (read_inode(inode_num, &inode) != 0)
            return -1;

        if (read_block(inode.dataPtr[0], buf2) != 0)
            return -1;

        strncpy(curname, (char *)buf2, SOFS_MAX_FILE_NAME_SIZE);
        curname[SOFS_MAX_FILE_NAME_SIZE] = '\0';
    }

    if (depth == MAX_LINK_DEPTH)
        return -1;  /* cycle or depth exceeded */

    handle = find_free_handle();
    if (handle < 0)
        return -1;

    g_open_files[handle - 1].in_use = 1;
    g_open_files[handle - 1].inodeNumber = inode_num;
    g_open_files[handle - 1].position = 0;

    return handle;
}

int sofs_close(SOFS_FILE handle)
{
    int idx;

    if (handle <= 0)
        return -1;

    idx = handle - 1;
    if (idx < 0 || idx >= SOFS_OPEN_FILE_MAX || !g_open_files[idx].in_use)
        return -1;

    memset(&g_open_files[idx], 0, sizeof(g_open_files[idx]));
    return 0;
}

int sofs_read(SOFS_FILE handle, char *buffer, int size)
{
    struct sofs_open_file *file;
    struct sofs_inode inode;
    DWORD bytes_read = 0;
    DWORD block_size;

    if (handle <= 0 || buffer == NULL || size <= 0)
        return -1;

    file = &g_open_files[handle - 1];
    if (!file->in_use)
        return -1;

    if (read_inode(file->inodeNumber, &inode) != 0)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    while (size > 0) {
        DWORD block_index = file->position / block_size;
        DWORD block_offset = file->position % block_size;
        DWORD bytes_left_in_file = inode.bytesFileSize > file->position
            ? inode.bytesFileSize - file->position
            : 0;
        DWORD bytes_to_copy;
        unsigned char block_buf[SECTOR_SIZE * 2];
        unsigned int abs_block;

        if (bytes_left_in_file == 0)
            break;

        if (block_index >= 2)
            break;

        abs_block = inode.dataPtr[block_index];
        if (abs_block == 0)
            break;

        if (read_block(abs_block, block_buf) != 0)
            return -1;

        bytes_to_copy = (DWORD)size;
        if (bytes_to_copy > bytes_left_in_file)
            bytes_to_copy = bytes_left_in_file;
        if (bytes_to_copy > block_size - block_offset)
            bytes_to_copy = block_size - block_offset;

        memcpy(buffer + bytes_read, block_buf + block_offset, bytes_to_copy);
        file->position += bytes_to_copy;
        bytes_read += bytes_to_copy;
        size -= (int)bytes_to_copy;
    }

    return (int)bytes_read;
}

int sofs_write(SOFS_FILE handle, char *buffer, int size)
{
    struct sofs_open_file *file;
    struct sofs_inode inode;
    DWORD bytes_written = 0;
    DWORD block_size;

    if (handle <= 0 || buffer == NULL || size <= 0)
        return -1;

    file = &g_open_files[handle - 1];
    if (!file->in_use)
        return -1;

    if (read_inode(file->inodeNumber, &inode) != 0)
        return -1;

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    while (size > 0) {
        DWORD block_index = file->position / block_size;
        DWORD block_offset = file->position % block_size;
        DWORD bytes_to_write;
        unsigned char block_buf[SECTOR_SIZE * 2];
        int abs_block;

        if (block_index >= 2)
            break;

        if (inode.dataPtr[block_index] == 0) {
            abs_block = alloc_data_block();
            if (abs_block < 0)
                break;
            inode.dataPtr[block_index] = (DWORD)abs_block;
            memset(block_buf, 0, block_size);
        } else {
            abs_block = (int)inode.dataPtr[block_index];
            if (read_block((unsigned int)abs_block, block_buf) != 0)
                return -1;
        }

        bytes_to_write = (DWORD)size;
        if (bytes_to_write > block_size - block_offset)
            bytes_to_write = block_size - block_offset;

        memcpy(block_buf + block_offset, buffer + bytes_written, bytes_to_write);
        if (write_block((unsigned int)abs_block, block_buf) != 0)
            return -1;

        file->position += bytes_to_write;
        bytes_written += bytes_to_write;
        size -= (int)bytes_to_write;

        if (file->position > inode.bytesFileSize)
            inode.bytesFileSize = file->position;
        if (inode.blocksFileSize < block_index + 1)
            inode.blocksFileSize = block_index + 1;
    }

    if (write_inode(file->inodeNumber, &inode) != 0)
        return -1;

    return (int)bytes_written;
}

/* -------------------------------------------------------------------------
 * Operações de diretório (TODO)
 * ---------------------------------------------------------------------- */

int sofs_opendir(void)
{
    /* TODO: verifica que uma partição está montada, posiciona o ponteiro de
     * entradas no primeiro registro válido do diretório raiz e retorna 0. */
    return -1;
}

int sofs_readdir(SOFS_DIRENT *dentry)
{
    /* TODO: lê o próximo registro válido do diretório em *dentry e avança o
     * ponteiro de entradas. Retorna valor diferente de zero ao fim do diretório. */
    (void)dentry;
    return -1;
}

int sofs_closedir(void)
{
    /* TODO: reinicia o ponteiro de entradas do diretório e retorna 0. */
    return -1;
}

/* -------------------------------------------------------------------------
 * Operações de link (TODO)
 * ---------------------------------------------------------------------- */

int sofs_sln(char *linkname, char *filename)
{
    unsigned int inode_num;
    int block;
    struct sofs_inode inode;
    unsigned int block_size;
    unsigned char *buf;
    int idx;

    if (!g_mounted || linkname == NULL || filename == NULL)
        return -1;
    if (linkname[0] == '\0' || filename[0] == '\0')
        return -1;
    if (strlen(linkname) > SOFS_MAX_FILE_NAME_SIZE)
        return -1;
    if (strlen(filename) > SOFS_MAX_FILE_NAME_SIZE)
        return -1;

    if (find_directory_entry(linkname, &inode_num, &idx) == 0)
        return -1;  /* linkname already exists */

    inode_num = (unsigned int)alloc_inode();
    if ((int)inode_num < 0)
        return -1;

    block = alloc_data_block();
    if (block < 0) {
        free_inode(inode_num);
        return -1;
    }

    if (read_inode(inode_num, &inode) != 0) {
        free_data_block((unsigned int)block);
        free_inode(inode_num);
        return -1;
    }

    inode.dataPtr[0]    = (DWORD)block;
    inode.blocksFileSize = 1;
    inode.bytesFileSize  = (DWORD)strlen(filename);
    inode.RefCounter     = 1;

    if (write_inode(inode_num, &inode) != 0) {
        free_data_block((unsigned int)block);
        free_inode(inode_num);
        return -1;
    }

    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    buf = (unsigned char *)__builtin_alloca(block_size);
    memset(buf, 0, block_size);
    memcpy(buf, filename, strlen(filename));

    if (write_block((unsigned int)block, buf) != 0) {
        free_data_block((unsigned int)block);
        free_inode(inode_num);
        return -1;
    }

    if (set_directory_entry(linkname, inode_num, TYPEVAL_LINK, NULL) != 0) {
        free_data_block((unsigned int)block);
        free_inode(inode_num);
        return -1;
    }

    return 0;
}

int sofs_hln(char *linkname, char *filename)
{
    unsigned int target_inode;
    unsigned int tmp_inode;
    struct sofs_inode inode;
    int idx;

    if (!g_mounted || linkname == NULL || filename == NULL)
        return -1;
    if (linkname[0] == '\0' || filename[0] == '\0')
        return -1;
    if (strlen(linkname) > SOFS_MAX_FILE_NAME_SIZE)
        return -1;

    if (find_directory_entry(linkname, &tmp_inode, &idx) == 0)
        return -1;  /* linkname already exists */

    if (find_directory_entry(filename, &target_inode, &idx) != 0)
        return -1;  /* filename not found */

    if (read_inode(target_inode, &inode) != 0)
        return -1;

    inode.RefCounter++;

    if (write_inode(target_inode, &inode) != 0)
        return -1;

    if (set_directory_entry(linkname, target_inode, TYPEVAL_REGULAR, NULL) != 0) {
        /* rollback RefCounter increment */
        if (read_inode(target_inode, &inode) == 0) {
            inode.RefCounter--;
            write_inode(target_inode, &inode);
        }
        return -1;
    }

    return 0;
}
