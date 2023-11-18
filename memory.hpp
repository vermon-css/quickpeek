#include <vector>
#include <cstring>

#include <link.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

inline unsigned long engine_binary;
inline unsigned long server_binary;

inline
void init_memory_modules() {
    constexpr auto callback = [](dl_phdr_info* info, size_t size, void*) -> int {
        const char* name = basename(info->dlpi_name);

        if (std::strcmp(name, "server_srv.so") == 0)
            server_binary = info->dlpi_addr;
        else if (std::strcmp(name, "engine_srv.so") == 0)
            engine_binary = info->dlpi_addr;

        return 0;
    };

    dl_iterate_phdr(callback, nullptr);
}


inline
void unprotect_all_memory() {
    constexpr auto callback = [](dl_phdr_info* info, size_t size, void*) -> int {
        const char* name = basename(info->dlpi_name);
        auto page = sysconf(_SC_PAGESIZE);

        if (std::strcmp(name, "server_srv.so") != 0 && std::strcmp(name, "engine_srv.so") != 0)
            return 0;

        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& seg = info->dlpi_phdr[i];

            if (seg.p_flags & PF_W)
                continue;

            auto ptr = info->dlpi_addr + seg.p_vaddr;
            auto len = seg.p_memsz;

            if (ptr % page) {
                len += ptr % page;
                ptr -= ptr % page;
            }

            if (len % page)
                len += page - (len % page);

            mprotect(reinterpret_cast<void*>(ptr), len, PROT_READ | PROT_WRITE | PROT_EXEC);
        }

        return 0;
    };

    dl_iterate_phdr(callback, nullptr);
}

inline
std::vector<void*> binary_symbols(const char* b, const char* const* s, int n) {
    std::vector<void*> v(n);

    void* h = dlopen(b, RTLD_NOW | RTLD_NOLOAD);
    if (!h) return v;

    struct stat file_stat;

    auto file = open(b, O_RDONLY);
    fstat(file, &file_stat);

    auto map_base = reinterpret_cast<uintptr_t>(mmap(nullptr, file_stat.st_size, PROT_READ, MAP_PRIVATE, file, 0));
    auto header = reinterpret_cast<Elf32_Ehdr*>(map_base);

    close(file);

    auto sections = reinterpret_cast<Elf32_Shdr*>(map_base + header->e_shoff);
    auto sections_count = header->e_shnum;

    Elf32_Shdr* symtab_hdr = nullptr;
    Elf32_Shdr* strtab_hdr = nullptr;

    for (uint16_t i = 0; i < sections_count; ++i) {
        auto& hdr = sections[i];

        if (hdr.sh_type == SHT_SYMTAB)
            symtab_hdr = &hdr;
        else if (hdr.sh_type == SHT_STRTAB)
            strtab_hdr = &hdr;
    }

    auto symtab = reinterpret_cast<Elf32_Sym*>(map_base + symtab_hdr->sh_offset);
    auto strtab = reinterpret_cast<const char*>(map_base + strtab_hdr->sh_offset);
    auto symbol_count = symtab_hdr->sh_size / symtab_hdr->sh_entsize;

    for (int i = 0; i < n; ++i) {
        void* addr = dlsym(h, s[i]);
        if (addr) {
            v[i] = addr;
            continue;
        }

        for (uint32_t m = 0; m < symbol_count; ++m) {
            if (std::strcmp(strtab + symtab[m].st_name, s[i]) == 0) {
                v[i] = reinterpret_cast<void*>(static_cast<link_map*>(h)->l_addr + symtab[m].st_value);
                break;
            }
        }
    }

    dlclose(h);
    munmap(reinterpret_cast<void*>(map_base), file_stat.st_size);

    return v;
}
