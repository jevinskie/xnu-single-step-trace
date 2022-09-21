#include "common.h"

#include <CoreSymbolication/CoreSymbolication.h>

fs::path image_info::log_path() const {
    const std::span<const uint8_t> trunc_digest{digest.data(), 4};
    return {fmt::format("macho-region-{:s}-{:02x}.bin", path.filename().string(),
                        fmt::join(trunc_digest, ""))};
}

MachORegions::MachORegions(task_t target_task) : m_target_task{target_task} {
    reset();
}

MachORegions::MachORegions(const log_region *region_buf, uint64_t num_regions,
                           std::map<sha256_t, std::vector<uint8_t>> &regions_bytes) {
    for (uint64_t i = 0; i < num_regions; ++i) {
        const char *path_ptr = (const char *)(region_buf + 1);
        std::string path{path_ptr, region_buf->path_len};
        image_info img_info{.base   = region_buf->base,
                            .size   = region_buf->size,
                            .slide  = region_buf->slide,
                            .path   = path,
                            .is_jit = (bool)region_buf->is_jit};
        memcpy(img_info.uuid, region_buf->uuid, sizeof(img_info.uuid));
        memcpy(img_info.digest.data(), region_buf->digest_sha256, img_info.digest.size());
        img_info.bytes = std::move(regions_bytes[img_info.digest]);
        assert(img_info.bytes.size() == img_info.size);
        m_regions.emplace_back(img_info);
        region_buf =
            (log_region *)((uint8_t *)region_buf + sizeof(*region_buf) + region_buf->path_len);
    }
    std::sort(m_regions.begin(), m_regions.end());
    create_hash();
}

void MachORegions::reset() {
    assert(m_target_task);
    if (m_target_task != mach_task_self()) {
        mach_check(task_suspend(m_target_task), "region reset suspend");
    }
    m_regions = get_dyld_image_infos(m_target_task);

    const auto cs = CSSymbolicatorCreateWithTask(m_target_task);
    assert(!CSIsNull(cs));
    for (auto &region : m_regions) {
        const auto sym_owner =
            CSSymbolicatorGetSymbolOwnerWithAddressAtTime(cs, region.base, kCSNow);
        assert(!CSIsNull(sym_owner));
        const auto uuid = CSSymbolOwnerGetCFUUIDBytes(sym_owner);
        memcpy(region.uuid, uuid, sizeof(region.uuid));
    }
    CSRelease(cs);

    for (auto &region : m_regions) {
        region.size   = roundup_pow2_mul(region.size, PAGE_SZ);
        region.bytes  = read_target(m_target_task, region.base, region.size);
        region.digest = get_sha256(region.bytes);
    }

    std::vector<uint64_t> region_bases;
    for (const auto &region : m_regions) {
        region_bases.emplace_back(region.base);
    }

    int num_jit_regions = 0;
    for (const auto &vm_region : get_vm_regions(m_target_task)) {
        if (!(vm_region.prot & VM_PROT_EXECUTE)) {
            continue;
        }
        if (!(vm_region.prot & VM_PROT_READ)) {
            fmt::print("found XO page at {:#018x}\n", vm_region.base);
        }
        if (std::find(region_bases.cbegin(), region_bases.cend(), vm_region.base) !=
            region_bases.cend()) {
            continue;
        }
        if (vm_region.tag != 0xFF) {
            continue;
        }
        const auto bytes = read_target(m_target_task, vm_region.base, vm_region.size);
        m_regions.emplace_back(image_info{
            .base  = vm_region.base,
            .size  = vm_region.size,
            .path  = fmt::format("/tmp/jit-region-{:d}-tag-{:02x}", num_jit_regions, vm_region.tag),
            .uuid  = {},
            .bytes = std::move(bytes),
            .digest = get_sha256(bytes),
            .is_jit = true});
        ++num_jit_regions;
    }

    std::sort(m_regions.begin(), m_regions.end());

    create_hash();

    if (m_target_task != mach_task_self()) {
        mach_check(task_resume(m_target_task), "region reset resume");
    }
}

const image_info &MachORegions::lookup(uint64_t addr) const {
    for (const auto &img_info : m_regions) {
        if (img_info.base <= addr && addr < img_info.base + img_info.size) {
            return img_info;
        }
    }
    assert(!"no region found");
}

std::pair<const image_info &, size_t> MachORegions::lookup_idx(uint64_t addr) const {
    size_t idx = 0;
    for (const auto &img_info : m_regions) {
        if (img_info.base <= addr && addr < img_info.base + img_info.size) {
            return std::make_pair(img_info, idx);
        }
        ++idx;
    }
    assert(!"no region found");
}

const image_info &MachORegions::lookup(const std::string &image_name) const {
    std::vector<const image_info *> matches;
    for (const auto &img_info : m_regions) {
        if (img_info.path.filename().string() == image_name) {
            matches.emplace_back(&img_info);
        }
    }
    assert(matches.size() == 1);
    return *matches[0];
}

uint32_t MachORegions::lookup_inst(uint64_t addr) const {
    const auto idx       = m_page_addr_hasher(addr >> PAGE_SZ_LOG2);
    const auto page_addr = rounddown_pow2_mul(addr, PAGE_SZ);
    const auto page_off  = addr - page_addr;
    return *(uint32_t *)(m_regions_bufs[idx] + page_off);
}

const std::vector<image_info> &MachORegions::regions() const {
    return m_regions;
}

void MachORegions::create_hash() {
    std::vector<uint64_t> page_addrs;
    for (const auto &region : m_regions) {
        assert(!(region.base & PAGE_SZ_MASK));
        assert(!(region.size & PAGE_SZ_MASK));
        for (size_t off = 0; off < region.size; off += PAGE_SZ) {
            page_addrs.emplace_back((region.base + off) >> PAGE_SZ_LOG2);
        }
    }
    std::sort(page_addrs.begin(), page_addrs.end());
    page_addrs.erase(std::unique(page_addrs.begin(), page_addrs.end()), page_addrs.end());
    pthash::build_configuration config;
    config.minimal_output = true;
    config.verbose_output = false;

    // fmt::print("building mph with {:d} keys\n", page_addrs.size());
    m_page_addr_hasher.build_in_internal_memory(page_addrs.begin(), page_addrs.size(), config);
    // fmt::print("mph building done\n");
    // const auto bits_per_key = (double)m_page_addr_hasher.num_bits() /
    // m_page_addr_hasher.num_keys(); fmt::print("function uses {:0.4f} bits/key\n", bits_per_key);
    m_regions_bufs.clear();
    m_regions_bufs.resize(page_addrs.size());
    // base regions
    for (const auto &region : m_regions) {
        for (size_t off = 0; off < region.size; off += PAGE_SZ) {
            const auto idx      = m_page_addr_hasher((region.base + off) >> PAGE_SZ_LOG2);
            m_regions_bufs[idx] = region.bytes.data() + off;
        }
    }
    // override jit regions
    for (const auto &region : m_regions) {
        if (!region.is_jit) {
            continue;
        }
        for (size_t off = 0; off < region.size; off += PAGE_SZ) {
            const auto idx      = m_page_addr_hasher((region.base + off) >> PAGE_SZ_LOG2);
            m_regions_bufs[idx] = region.bytes.data() + off;
        }
    }
}

void MachORegions::dump() const {
    for (const auto &region : m_regions) {
        fmt::print("base: {:#018x} => {:#018x} size: {:#010x} slide: {:#x} path: '{:s}'\n",
                   region.base, region.base + region.size, region.size, region.slide,
                   region.path.string());
    }
}