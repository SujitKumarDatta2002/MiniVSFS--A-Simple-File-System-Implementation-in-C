// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_adder_skeleton.c -o mkfs_adder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <getopt.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
#pragma pack(push, 1)

typedef struct {
    uint32_t magic;              // 0x4D565346
    uint32_t version;            // 1
    uint32_t block_size;         // 4096
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;         // 1
    uint64_t mtime_epoch;        // Build time
    uint32_t flags;              // 0
    uint32_t checksum;           // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;              
    uint16_t links;            
    uint32_t uid;              
    uint32_t gid;               
    uint64_t size_bytes;   
    uint64_t atime;            
    uint64_t mtime;          
    uint64_t ctime;          
    uint32_t direct[DIRECT_MAX]; 
    uint32_t reserved_0;         
    uint32_t reserved_1;         
    uint32_t reserved_2;       
    uint32_t proj_id;        
    uint32_t uid16_gid16;       
    uint64_t xattr_ptr;          
    uint64_t inode_crc;          // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;           
    uint8_t  type;               // 1=file, 2=dir
    char     name[58];           
    uint8_t  checksum;           // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    (*sb).checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    (*sb).checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    (*ino).inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    (*de).checksum = x;
}

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s --input <input_image> --output <output_image> --file <filename>\n", prog_name);
}

uint32_t find_first_free_bit(uint8_t *bitmap, uint32_t bitmap_size, uint32_t max_items) {
    for (uint32_t byte = 0; byte < bitmap_size && byte * 8 < max_items; byte++) {
        for (int bit = 0; bit < 8 && byte * 8 + bit < max_items; bit++) {
            if (!(bitmap[byte] & (1 << bit))) {
                return byte * 8 + bit + 1;
            }
        }
    }
    return 0;
}

void set_bit(uint8_t *bitmap, uint32_t bit_index) {
    uint32_t byte_index = (bit_index - 1) / 8;
    uint32_t bit_offset = (bit_index - 1) % 8;
    bitmap[byte_index] |= (1 << bit_offset);
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    char *input_name = NULL;
    char *output_name = NULL;
    char *file_name = NULL;
    
    struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"file", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                input_name = optarg;
                break;
            case 'o':
                output_name = optarg;
                break;
            case 'f':
                file_name = optarg;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    if (!input_name || !output_name || !file_name) {
        fprintf(stderr, "Error: Missing required arguments\n");
        print_usage(argv[0]);
        return 1;
    }
    
    FILE *file_fp = fopen(file_name, "rb");
    if (!file_fp) {
        fprintf(stderr, "Error: Cannot open file %s: %s\n", file_name, strerror(errno));
        return 1;
    }
    
    fseek(file_fp, 0, SEEK_END);
    long file_size = ftell(file_fp);
    fseek(file_fp, 0, SEEK_SET);
    
    if (file_size < 0) {
        fprintf(stderr, "Error: Cannot determine file size\n");
        fclose(file_fp);
        return 1;
    }
    
    uint64_t blocks_needed = (file_size + BS - 1) / BS;
    if (blocks_needed > DIRECT_MAX) {
        fprintf(stderr, "Error: File too large to fit in %d direct blocks\n", DIRECT_MAX);
        fclose(file_fp);
        return 1;
    }
    
    FILE *input_fp = fopen(input_name, "rb");
    if (!input_fp) {
        fprintf(stderr, "Error: Cannot open input image %s: %s\n", input_name, strerror(errno));
        fclose(file_fp);
        return 1;
    }
    
    superblock_t sb;
    if (fread(&sb, 1, sizeof(sb), input_fp) != sizeof(sb)) {
        fprintf(stderr, "Error reading superblock\n");
        fclose(file_fp);
        fclose(input_fp);
        return 1;
    }
    
    if (sb.magic != 0x4D565346) {
        fprintf(stderr, "Error: Invalid filesystem magic number\n");
        fclose(file_fp);
        fclose(input_fp);
        return 1;
    }
    
    if (blocks_needed > sb.data_region_blocks) {
        fprintf(stderr, "Error: Not enough free data blocks\n");
        fclose(file_fp);
        fclose(input_fp);
        return 1;
    }
    
    fseek(input_fp, 0, SEEK_END);
    long image_size = ftell(input_fp);
    fseek(input_fp, 0, SEEK_SET);
    
    uint8_t *fs_image = malloc(image_size);
    if (!fs_image) {
        fprintf(stderr, "Error: Cannot allocate memory for filesystem image\n");
        fclose(file_fp);
        fclose(input_fp);
        return 1;
    }
    
    if (fread(fs_image, 1, image_size, input_fp) != image_size) {
        fprintf(stderr, "Error reading filesystem image\n");
        free(fs_image);
        fclose(file_fp);
        fclose(input_fp);
        return 1;
    }
    fclose(input_fp);
    
    uint8_t *inode_bitmap = fs_image + sb.inode_bitmap_start * BS;
    uint8_t *data_bitmap = fs_image + sb.data_bitmap_start * BS;
    uint8_t *inode_table = fs_image + sb.inode_table_start * BS;
    uint8_t *data_region = fs_image + sb.data_region_start * BS;
    
    uint32_t new_inode_no = find_first_free_bit(inode_bitmap, BS, sb.inode_count);
    if (new_inode_no == 0) {
        fprintf(stderr, "Error: No free inodes available\n");
        free(fs_image);
        fclose(file_fp);
        return 1;
    }
    
    uint32_t data_blocks[DIRECT_MAX] = {0};
    uint32_t blocks_found = 0;
    
    for (uint32_t i = 1; i <= sb.data_region_blocks && blocks_found < blocks_needed; i++) {
        uint32_t byte_idx = (i - 1) / 8;
        uint32_t bit_idx = (i - 1) % 8;
        if (!(data_bitmap[byte_idx] & (1 << bit_idx))) {
            data_blocks[blocks_found] = sb.data_region_start + i - 1;
            blocks_found++;
        }
    }
    
    if (blocks_found < blocks_needed) {
        fprintf(stderr, "Error: Not enough free data blocks\n");
        free(fs_image);
        fclose(file_fp);
        return 1;
    }
    
    inode_t new_inode = {0};
    new_inode.mode = 0100000; 
    new_inode.links = 1;
    new_inode.uid = 0;
    new_inode.gid = 0;
    new_inode.size_bytes = file_size;
    time_t now = time(NULL);
    new_inode.atime = now;
    new_inode.mtime = now;
    new_inode.ctime = now;
    
    for (uint32_t i = 0; i < blocks_needed; i++) {
        new_inode.direct[i] = data_blocks[i];
    }
    for (uint32_t i = blocks_needed; i < DIRECT_MAX; i++) {
        new_inode.direct[i] = 0;
    }
    
    new_inode.reserved_0 = 0;
    new_inode.reserved_1 = 0;
    new_inode.reserved_2 = 0;
    new_inode.proj_id = 13; 
    new_inode.uid16_gid16 = 0;
    new_inode.xattr_ptr = 0;
    
    for (uint32_t i = 0; i < blocks_needed; i++) {
        uint8_t block_data[BS] = {0};
        size_t to_read = (i == blocks_needed - 1) ? 
            (file_size - i * BS) : BS;
        
        if (fread(block_data, 1, to_read, file_fp) != to_read) {
            fprintf(stderr, "Error reading file data\n");
            free(fs_image);
            fclose(file_fp);
            return 1;
        }
        
        uint64_t block_offset = (data_blocks[i] * BS);
        memcpy(fs_image + block_offset, block_data, BS);
    }
    fclose(file_fp);
    
    set_bit(inode_bitmap, new_inode_no);
    for (uint32_t i = 0; i < blocks_needed; i++) {
        uint32_t data_block_idx = data_blocks[i] - sb.data_region_start + 1;
        set_bit(data_bitmap, data_block_idx);
    }
    
    inode_crc_finalize(&new_inode);
    uint64_t inode_offset = (new_inode_no - 1) * INODE_SIZE;
    memcpy(inode_table + inode_offset, &new_inode, sizeof(new_inode));
    
    inode_t *root_inode = (inode_t *)inode_table;
    
    uint8_t *root_dir_data = data_region + ((*root_inode).direct[0] - sb.data_region_start) * BS;
    
    dirent64_t *entries = (dirent64_t *)root_dir_data;
    int entries_per_block = BS / sizeof(dirent64_t);
    int free_entry = -1;
    
    for (int i = 0; i < entries_per_block; i++) {
        if (entries[i].inode_no == 0) {
            free_entry = i;
            break;
        }
    }
    
    if (free_entry == -1) {
        fprintf(stderr, "Error: No free directory entries in root directory\n");
        free(fs_image);
        return 1;
    }
    
    dirent64_t new_entry = {0};
    new_entry.inode_no = new_inode_no;
    new_entry.type = 1; // file
    
    char *basename = strrchr(file_name, '/');
    if (basename) {
        basename++; 
    } else {
        basename = file_name;
    }
    
    if (strlen(basename) >= 58) {
        fprintf(stderr, "Error: Filename too long (max 57 characters)\n");
        free(fs_image);
        return 1;
    }
    
    strcpy(new_entry.name, basename);
    dirent_checksum_finalize(&new_entry);
    
    memcpy(&entries[free_entry], &new_entry, sizeof(new_entry));
    
    (*root_inode).size_bytes += sizeof(dirent64_t);
    (*root_inode).links++; 
    (*root_inode).mtime = now;
    inode_crc_finalize(root_inode);
    
    superblock_t *sb_ptr = (superblock_t *)fs_image;
    superblock_crc_finalize(sb_ptr);
    
    FILE *output_fp = fopen(output_name, "wb");
    if (!output_fp) {
        fprintf(stderr, "Error: Cannot create output file %s: %s\n", output_name, strerror(errno));
        free(fs_image);
        return 1;
    }
    
    if (fwrite(fs_image, 1, image_size, output_fp) != image_size) {
        fprintf(stderr, "Error writing output image\n");
        free(fs_image);
        fclose(output_fp);
        return 1;
    }
    
    fclose(output_fp);
    free(fs_image);
    
    printf("File '%s' successfully added to filesystem\n", basename);
    printf("Assigned inode number: %u\n", new_inode_no);
    printf("File size: %ld bytes\n", file_size);
    printf("Blocks used: %u\n", (uint32_t)blocks_needed);
    
    return 0;
}