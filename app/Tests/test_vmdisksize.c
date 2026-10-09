#include "VMDiskSize.h"
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    uint64_t size = 123;
    CHECK(!vm_disk_size_record(0)); CHECK(!vm_disk_size_record(1));
    CHECK(!vm_disk_size_record(3)); CHECK(!vm_disk_size_record(16));
    for (unsigned gib = 2; gib <= 8; gib *= 2) {
        const char *r = vm_disk_size_record(gib);
        CHECK(vm_disk_size_parse(r,strlen(r),&size));
        CHECK(size == gib * VM_DISK_GIB);
        size = 123;
        CHECK(!vm_disk_size_parse(r,strlen(r)-1,&size)); CHECK(size == 123);
        CHECK(!vm_disk_size_parse(r,strlen(r)+1,&size));
    }
    CHECK(!vm_disk_size_parse("s5lbox-disk-v1 3\n",strlen("s5lbox-disk-v1 3\n"),&size));
    CHECK(!vm_disk_size_parse("s5lbox-disk-v2 2\n",strlen("s5lbox-disk-v2 2\n"),&size));
    CHECK(!vm_disk_size_parse(NULL,0,&size));
    CHECK(!vm_disk_size_read(NULL,&size));
    CHECK(!vm_disk_size_read("",&size));
    CHECK(vm_disk_size_read("missing-disk-config-test-directory",&size));
    CHECK(size == 0);
    /* Test-directory cwd, never a machine directory. */
    FILE *f = fopen(VM_DISK_SIZE_FILE,"wb"); CHECK(f);
    size_t record_size = strlen(vm_disk_size_record(8));
    CHECK(fwrite(vm_disk_size_record(8),1,record_size,f) == record_size); CHECK(fclose(f) == 0);
    CHECK(vm_disk_size_read(".",&size)); CHECK(size == 8 * VM_DISK_GIB);
    f = fopen(VM_DISK_SIZE_FILE,"ab"); CHECK(f); CHECK(fputc(0,f) == 0); CHECK(fclose(f) == 0);
    size = 123; CHECK(!vm_disk_size_read(".",&size)); CHECK(size == 123);
    CHECK(remove(VM_DISK_SIZE_FILE) == 0);
    puts("disk-size records: strict 2/4/8 GiB and legacy/malformed cases passed");
    return 0;
}
