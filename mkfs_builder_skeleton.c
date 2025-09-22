// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder_skeleton.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <getopt.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

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
    // CREATE YOUR INODE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE

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


    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
        
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
    fprintf(stderr, "Usage: %s --image <filename> --size-kib <180..4096> --inodes <128..512>\n", prog_name);
}

int main(int argc, char *argv[]) {
    crc32_init();
    
    char *image_name = NULL;
    uint64_t size_kib = 0;
    uint64_t inode_count = 0;
    
    struct option long_options[] = {
        {"image", required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes", required_argument, 0, 'n'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                image_name = optarg;
                break;
            case 's':
                size_kib = strtoull(optarg, NULL, 10);
                break;
            case 'n':
                inode_count = strtoull(optarg, NULL, 10);
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    if (!image_name || size_kib == 0 || inode_count == 0) {
        fprintf(stderr, "Error: Missing required arguments\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (size_kib < 180 || size_kib > 4096 || size_kib % 4 != 0) {
        fprintf(stderr, "Error: size-kib must be between 180-4096 and multiple of 4\n");
        return 1;
    }
    
    if (inode_count < 128 || inode_count > 512) {
        fprintf(stderr, "Error: inodes must be between 128-512\n");
        return 1;
    }
    

    uint64_t total_blocks = (size_kib * 1024) / BS;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    
    uint64_t metadata_blocks = 1 + 1 + 1 + inode_table_blocks; // superblock + inode bitmap + data bitmap + inode table
    if (total_blocks <= metadata_blocks) {
        fprintf(stderr, "Error: Not enough blocks for filesystem metadata\n");
        return 1;
    }
    
    uint64_t data_region_blocks = total_blocks - metadata_blocks;
    
    superblock_t sb = {0};
    sb.magic = 0x4D565346;
    sb.version = 1;
    sb.block_size = BS;
    sb.total_blocks = total_blocks;
    sb.inode_count = inode_count;
    sb.inode_bitmap_start = 1;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = 2;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = 3;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_region_start = 3 + inode_table_blocks;
    sb.data_region_blocks = data_region_blocks;
    sb.root_inode = ROOT_INO;
    sb.mtime_epoch = time(NULL);
    sb.flags = 0;
    
    inode_t root_inode = {0};
    root_inode.mode = 0040000; 
    root_inode.links = 2; 
    root_inode.uid = 0;
    root_inode.gid = 0;
    root_inode.size_bytes = 2 * sizeof(dirent64_t); 
    root_inode.atime = sb.mtime_epoch;
    root_inode.mtime = sb.mtime_epoch;
    root_inode.ctime = sb.mtime_epoch;
    root_inode.direct[0] = sb.data_region_start; 
    for (int i = 1; i < DIRECT_MAX; i++) {
        root_inode.direct[i] = 0;
    }
    root_inode.reserved_0 = 0;
    root_inode.reserved_1 = 0;
    root_inode.reserved_2 = 0;
    root_inode.proj_id = 13; // my group no is 13
    root_inode.uid16_gid16 = 0;
    root_inode.xattr_ptr = 0;
    
    dirent64_t dot_entry = {0};
    dot_entry.inode_no = ROOT_INO;
    dot_entry.type = 2; // directory
    strcpy(dot_entry.name, ".");
    
    dirent64_t dotdot_entry = {0};
    dotdot_entry.inode_no = ROOT_INO;
    dotdot_entry.type = 2; // directory
    strcpy(dotdot_entry.name, "..");
    
    FILE *fp = fopen(image_name, "wb");
    if (!fp) {
        fprintf(stderr, "Error: Cannot create file %s: %s\n", image_name, strerror(errno));
        return 1;
    }
    
    superblock_crc_finalize(&sb);
    uint8_t block[BS] = {0};
    memcpy(block, &sb, sizeof(sb));
    if (fwrite(block, 1, BS, fp) != BS) {
        fprintf(stderr, "Error writing superblock\n");
        fclose(fp);
        return 1;
    }
    
    memset(block, 0, BS);
    block[0] = 0x01; 
    if (fwrite(block, 1, BS, fp) != BS) {
        fprintf(stderr, "Error writing inode bitmap\n");
        fclose(fp);
        return 1;
    }
    
    memset(block, 0, BS);
    block[0] = 0x01;
    if (fwrite(block, 1, BS, fp) != BS) {
        fprintf(stderr, "Error writing data bitmap\n");
        fclose(fp);
        return 1;
    }
    
    for (uint64_t i = 0; i < inode_table_blocks; i++) {
        memset(block, 0, BS);
        if (i == 0) {
            inode_crc_finalize(&root_inode);
            memcpy(block, &root_inode, sizeof(root_inode));
        }
        if (fwrite(block, 1, BS, fp) != BS) {
            fprintf(stderr, "Error writing inode table block %llu\n", (unsigned long long)i);
            fclose(fp);
            return 1;
        }
    }
    
    for (uint64_t i = 0; i < data_region_blocks; i++) {
        memset(block, 0, BS);
        if (i == 0) {
            dirent_checksum_finalize(&dot_entry);
            dirent_checksum_finalize(&dotdot_entry);
            memcpy(block, &dot_entry, sizeof(dot_entry));
            memcpy(block + sizeof(dot_entry), &dotdot_entry, sizeof(dotdot_entry));
        }
        if (fwrite(block, 1, BS, fp) != BS) {
            fprintf(stderr, "Error writing data block %llu\n", (unsigned long long)i);
            fclose(fp);
            return 1;
        }
    }
    
    fclose(fp);
    printf("MiniVSFS image '%s' created successfully\n", image_name);
    printf("Total blocks: %llu\n", (unsigned long long)total_blocks);
    printf("Inode count: %llu\n", (unsigned long long)inode_count);
    printf("Data blocks: %llu\n", (unsigned long long)data_region_blocks);
    
    return 0;
}