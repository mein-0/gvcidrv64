#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#pragma comment(lib, "advapi32.lib")

#define DEVICE_PATH "\\\\.\\GVCIDrv64"
#define SVC_NAME    "GVCIDrv64"
#define IOCTL_MAP   0x9C406580
#define IOCTL_UNMAP 0x9C406584
#define IOCTL_PORT  0x9C406588
#define MAP_SIZE    0x2000000ULL  

#define VBUS 0x03
#define VDEV 0x00

typedef struct { USHORT port; USHORT pad; DWORD data; USHORT dir; USHORT size; } PIO;
static HANDLE hDev;
static DWORD io_in(USHORT p, USHORT s) {
    PIO i={0},o={0}; DWORD br;
    i.port=p; i.size=s;
    DeviceIoControl(hDev,IOCTL_PORT,&i,12,&o,12,&br,NULL);
    return o.data;
}
static void io_out(USHORT p, DWORD v, USHORT s) {
    PIO i={0},o={0}; DWORD br;
    i.port=p; i.size=s; i.dir=1; i.data=v;
    DeviceIoControl(hDev,IOCTL_PORT,&i,12,&o,12,&br,NULL);
}
static DWORD pcir(int b,int d,int f,int r) {
    io_out(0xCF8, 0x80000000|(b<<16)|(d<<11)|(f<<8)|(r&0xFC), 4);
    return io_in(0xCFC, 4);
}
static void pciw(int b,int d,int f,int r,DWORD v) {
    io_out(0xCF8, 0x80000000|(b<<16)|(d<<11)|(f<<8)|(r&0xFC), 4);
    io_out(0xCFC, v, 4);
}

static uint64_t do_map(BYTE bus, BYTE dev) {
    BYTE in[4]={bus,dev,0,0}; uint64_t out=0; DWORD br=0;
    DeviceIoControl(hDev, IOCTL_MAP, in, 4, &out, 8, &br, NULL);
    return out;
}
static void do_unmap(uint64_t p) {
    if(!p) return; DWORD br=0;
    DeviceIoControl(hDev, IOCTL_UNMAP, &p, 8, NULL, 0, &br, NULL);
}

static void drv_load(void) {
    char sys[MAX_PATH];
    GetModuleFileNameA(NULL, sys, MAX_PATH);
    char *s = strrchr(sys, '\\');
    if(s) strcpy(s+1, "GVCIDrv64.sys");

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    SC_HANDLE svc = OpenServiceA(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if(!svc) svc = CreateServiceA(scm, SVC_NAME, SVC_NAME, SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, sys, NULL, NULL, NULL, NULL, NULL);
    StartServiceA(svc, 0, NULL);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

static DWORD saved_bar0, saved_cmd;

static uint8_t* phys_map(uint64_t phys_addr) {
    DWORD base = (DWORD)(phys_addr & ~(MAP_SIZE - 1));

    saved_bar0 = pcir(VBUS, VDEV, 0, 0x10);
    saved_cmd  = pcir(VBUS, VDEV, 0, 0x04);
    pciw(VBUS, VDEV, 0, 0x04, saved_cmd & ~0x07);

    pciw(VBUS, VDEV, 0, 0x10, base);

    uint64_t va = do_map(VBUS, VDEV);
    if(!va) {
        pciw(VBUS, VDEV, 0, 0x10, saved_bar0);
        pciw(VBUS, VDEV, 0, 0x04, saved_cmd);
        return NULL;
    }

    return (uint8_t*)(uintptr_t)va;
}

static void phys_unmap(uint8_t *va) {
    if(va) do_unmap((uint64_t)(uintptr_t)va);
    pciw(VBUS, VDEV, 0, 0x10, saved_bar0);
    pciw(VBUS, VDEV, 0, 0x04, saved_cmd);
}


static DWORD phys_read32(uint64_t phys_addr) {
    uint8_t *base = phys_map(phys_addr);
    if(!base) { printf("map fail\n"); return 0xDEADDEAD; }

    DWORD offset = (DWORD)(phys_addr & (MAP_SIZE - 1));
    DWORD val = *(volatile DWORD*)(base + offset);

    phys_unmap(base);
    return val;
}

static void phys_write32(uint64_t phys_addr, DWORD value) {
    uint8_t *base = phys_map(phys_addr);
    if(!base) { printf("map fail\n"); return; }

    DWORD offset = (DWORD)(phys_addr & (MAP_SIZE - 1));
    *(volatile DWORD*)(base + offset) = value;

    phys_unmap(base);
}

static void phys_dump(uint64_t phys_addr, DWORD length) {
    uint8_t *base = phys_map(phys_addr);
    if(!base) { printf("map fail\n"); return; }

    DWORD offset = (DWORD)(phys_addr & (MAP_SIZE - 1));
    DWORD max_len = (DWORD)(MAP_SIZE - offset);
    if(length > max_len) length = max_len;

    for(DWORD i = 0; i < length; i += 16) {
        printf("  %08llX: ", (unsigned long long)(phys_addr + i));
        for(DWORD j = 0; j < 16 && (i+j) < length; j += 4) {
            DWORD val = *(volatile DWORD*)(base + offset + i + j);
            printf("%08X ", val);
        }
        printf("\n");
    }

    phys_unmap(base);
}


int main(int argc, char **argv) {
    if(argc < 3) {
        printf("usage: %s <read|write|dump> <phys_addr> [value|len]\n", argv[0]);
        CloseHandle(hDev);
        return 1;
    }
    

    drv_load();
    hDev = CreateFileA(DEVICE_PATH, 0xC0000000, 0, NULL, 3, 0, NULL);
    if(hDev == INVALID_HANDLE_VALUE) {
        printf("driver open fail %lu\n", GetLastError());
        return 1;
    }

    DWORD nic_id = pcir(VBUS, VDEV, 0, 0);
    printf("nic pci id: %04X:%04X\n", nic_id & 0xFFFF, nic_id >> 16);
    printf("nic bar0: 0x%08X  CMD: 0x%04X\n\n", pcir(VBUS,VDEV,0,0x10), pcir(VBUS,VDEV,0,0x04) & 0xFFFF);

    uint64_t addr = (uint64_t)strtoull(argv[2], NULL, 16);

    if(_stricmp(argv[1], "read") == 0) {
        DWORD val = phys_read32(addr);
        printf("phys[0x%llX] = 0x%08X\n", (unsigned long long)addr, val);
    }
    else if(_stricmp(argv[1], "write") == 0) {
        if(argc < 4) { printf("value not\n"); return 1; }
        DWORD val = (DWORD)strtoul(argv[3], NULL, 16);
        printf("phys[0x%llX] <- 0x%08X\n", (unsigned long long)addr, val);
        phys_write32(addr, val);
        printf("writed\n");
        DWORD rb = phys_read32(addr);
        printf("verify: 0x%08X %s\n", rb, (rb == val) ? "OK" : "different");
    }
    else if(_stricmp(argv[1], "dump") == 0) {
        DWORD len = (argc >= 4) ? (DWORD)strtoul(argv[3], NULL, 16) : 0x80;
        printf("fump: phys 0x%llX, %u bytes:\n\n", (unsigned long long)addr, len);
        phys_dump(addr, len);
    }
    else {
        printf("?????: %s\n", argv[1]);
    }

    printf("\nNIC restore BAR0=0x%08X CMD=0x%04X\n", pcir(VBUS,VDEV,0,0x10), pcir(VBUS,VDEV,0,0x04) & 0xFFFF);

    CloseHandle(hDev);
    return 0;
}