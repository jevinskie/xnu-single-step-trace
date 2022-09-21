#include "common.h"

#include <set>

std::vector<bb_t> extract_bbs_from_pc_trace(const std::span<const uint64_t> &pcs) {
    std::vector<bb_t> bbs;

    uint64_t bb_start = pcs[0];
    uint64_t last_pc  = pcs[0] - 4;
    for (const auto pc : pcs) {
        if (last_pc + 4 != pc) {
            bbs.emplace_back(bb_t{.pc = bb_start, .sz = (uint32_t)(last_pc + 4 - bb_start)});
            bb_start = pc;
        }
        last_pc = pc;
    }
    if (bb_start != last_pc) {
        bbs.emplace_back(bb_t{.pc = bb_start, .sz = (uint32_t)(last_pc + 4 - bb_start)});
    }
    return bbs;
}

std::vector<uint64_t> extract_pcs_from_trace(const std::span<const log_msg_hdr> &msgs) {
    std::vector<uint64_t> pcs;
    pcs.resize(msgs.size());
    size_t i = 0;
    for (const auto &msg : msgs) {
        pcs[i++] = msg.pc;
    }
    return pcs;
}

TraceLog::TraceLog(const std::string &log_dir_path, int compression_level, bool stream)
    : m_log_dir_path{log_dir_path}, m_compression_level{compression_level}, m_stream{stream} {
    fs::create_directory(m_log_dir_path);
    for (const auto &dirent : std::filesystem::directory_iterator{m_log_dir_path}) {
        if (!dirent.path().filename().string().starts_with("macho-region-")) {
            fs::remove(dirent.path());
        }
    }
}

TraceLog::TraceLog(const std::string &log_dir_path) : m_log_dir_path{log_dir_path} {
    CompressedFile<log_meta_hdr> meta_fh{m_log_dir_path / "meta.bin", true, log_meta_hdr_magic};
    const auto meta_buf = meta_fh.read();
    const auto meta_hdr = meta_fh.header();

    std::map<sha256_t, std::vector<uint8_t>> regions_bytes;
    for (const auto &dirent : std::filesystem::directory_iterator{log_dir_path}) {
        if (!dirent.path().filename().string().starts_with("macho-region-")) {
            continue;
        }
        CompressedFile<log_macho_region_hdr> region_fh{dirent.path(), true,
                                                       log_macho_region_hdr_magic};
        sha256_t digest;
        memcpy(digest.data(), region_fh.header().digest_sha256, digest.size());
        regions_bytes.emplace(digest, region_fh.read());
    }

    auto region_ptr = (log_region *)meta_buf.data();
    m_macho_regions =
        std::make_unique<MachORegions>(region_ptr, meta_hdr.num_regions, regions_bytes);
    for (uint64_t i = 0; i < meta_hdr.num_regions; ++i) {
        region_ptr =
            (log_region *)((uint8_t *)region_ptr + sizeof(*region_ptr) + region_ptr->path_len);
    }

    auto syms_ptr = (log_sym *)region_ptr;
    m_symbols     = std::make_unique<Symbols>(syms_ptr, meta_hdr.num_syms);
    for (uint64_t i = 0; i < meta_hdr.num_syms; ++i) {
        syms_ptr = (log_sym *)((uint8_t *)syms_ptr + sizeof(*syms_ptr) + syms_ptr->name_len +
                               syms_ptr->path_len);
    }

    for (const auto &dirent : std::filesystem::directory_iterator{m_log_dir_path}) {
        const auto fn = dirent.path().filename();
        if (fn == "meta.bin" || fn.string().starts_with("macho-region-")) {
            continue;
        }
        assert(fn.string().starts_with("thread-"));

        CompressedFile<log_thread_hdr> thread_fh{dirent.path(), true, log_thread_hdr_magic};
        const auto thread_buf = thread_fh.read();
        const auto thread_hdr = thread_fh.header();

        std::vector<log_msg_hdr> thread_log;
        auto inst_hdr           = (log_msg_hdr *)thread_buf.data();
        const auto inst_hdr_end = (log_msg_hdr *)(thread_buf.data() + thread_buf.size());
        thread_log.resize(thread_hdr.num_inst);
        size_t i = 0;
        while (inst_hdr < inst_hdr_end) {
            thread_log[i++] = *inst_hdr;
            ++inst_hdr;
        }
        m_num_inst += thread_hdr.num_inst;
        m_parsed_logs.emplace(thread_hdr.thread_id, std::move(thread_log));
    }
}

uint64_t TraceLog::num_inst() const {
    return m_num_inst;
}

size_t TraceLog::num_bytes() const {
    size_t sz = 0;
    if (!m_stream) {
        for (const auto &thread_log : m_log_bufs) {
            sz += thread_log.second.size();
        }
    } else {
        for (const auto &thread_stream : m_log_streams) {
            sz += thread_stream.second->decompressed_size();
        }
    }
    return sz;
}

const MachORegions &TraceLog::macho_regions() const {
    assert(m_macho_regions);
    return *m_macho_regions;
}

const Symbols &TraceLog::symbols() const {
    assert(m_symbols);
    return *m_symbols;
}

const std::map<uint32_t, std::vector<log_msg_hdr>> &TraceLog::parsed_logs() const {
    return m_parsed_logs;
}

__attribute__((always_inline)) void TraceLog::log(thread_t thread, uint64_t pc) {
    const auto msg_hdr = log_msg_hdr{.pc = pc};
    if (!m_stream) {
        std::copy((uint8_t *)&msg_hdr, (uint8_t *)&msg_hdr + sizeof(msg_hdr),
                  std::back_inserter(m_log_bufs[thread]));
    } else {
        if (!m_log_streams.contains(thread)) {
            const log_thread_hdr thread_hdr{.thread_id = thread};
            m_log_streams.emplace(
                thread, std::make_unique<CompressedFile<log_thread_hdr>>(
                            m_log_dir_path / fmt::format("thread-{:d}.bin", thread), false,
                            log_thread_hdr_magic, &thread_hdr, m_compression_level));
        }
        m_log_streams[thread]->write(msg_hdr);
    }
    m_thread_num_inst[thread] += 1;
    ++m_num_inst;
}

void TraceLog::write(const MachORegions &macho_regions, const Symbols *symbols) {
    std::set<uint64_t> pcs;
    for (const auto &thread_buf_pair : m_log_bufs) {
        const auto buf = thread_buf_pair.second;
        for (const auto pc : extract_pcs_from_trace(
                 {(log_msg_hdr *)buf.data(), buf.size() / sizeof(log_msg_hdr)})) {
            pcs.emplace(pc);
        }
    }
    interval_tree_t<uint64_t> pc_intervals;
    for (const auto pc : pcs) {
        pc_intervals.insert_overlap({pc, pc + 4});
    }

    std::vector<sym_info> syms;
    if (symbols) {
        const auto all_syms = symbols->syms();
        syms                = get_symbols_in_intervals(all_syms, pc_intervals);
    }

    const log_meta_hdr meta_hdr_buf{.num_regions = macho_regions.regions().size(),
                                    .num_syms    = syms.size()};
    CompressedFile<log_meta_hdr> meta_fh{m_log_dir_path / "meta.bin", false, log_meta_hdr_magic,
                                         &meta_hdr_buf, 0};

    for (const auto &region : macho_regions.regions()) {
        log_region region_buf{.base     = region.base,
                              .size     = region.size,
                              .slide    = region.slide,
                              .path_len = region.path.string().size(),
                              .is_jit   = region.is_jit};
        memcpy(region_buf.uuid, region.uuid, sizeof(region_buf.uuid));
        memcpy(region_buf.digest_sha256, region.digest.data(), sizeof(region_buf.digest_sha256));
        meta_fh.write(region_buf);
        meta_fh.write(region.path.c_str(), region.path.string().size());
    }

    for (const auto &sym : syms) {
        log_sym sym_buf{.base     = sym.base,
                        .size     = sym.size,
                        .name_len = sym.name.size(),
                        .path_len = sym.path.string().size()};
        meta_fh.write(sym_buf);
        meta_fh.write(sym.name.c_str(), sym.name.size());
        meta_fh.write(sym.path.c_str(), sym.path.string().size());
    }

    // find macho-region-*.bin that are unchanged
    std::set<fs::path> reused_macho_regions;
    for (const auto &region : macho_regions.regions()) {
        const auto old_region = m_log_dir_path / region.log_path();
        if (!fs::exists(old_region)) {
            continue;
        }
        CompressedFile<log_macho_region_hdr> old_region_fh{old_region, true,
                                                           log_macho_region_hdr_magic};
        if (!memcmp(old_region_fh.header().digest_sha256, region.digest.data(),
                    region.digest.size())) {
            reused_macho_regions.emplace(old_region);
        }
    }

    // remove all macho-regions-*.bin that aren't reused
    for (const auto &dirent : std::filesystem::directory_iterator{m_log_dir_path}) {
        if (dirent.path().filename().string().starts_with("macho-region-") &&
            !reused_macho_regions.contains(dirent.path())) {
            fs::remove(dirent.path());
        }
    }

    for (const auto &region : macho_regions.regions()) {
        const auto region_path = m_log_dir_path / region.log_path();
        if (reused_macho_regions.contains(region_path)) {
            continue;
        }
        log_macho_region_hdr macho_region_hdr_buf{};
        memcpy(macho_region_hdr_buf.digest_sha256, region.digest.data(), region.digest.size());
        CompressedFile<log_macho_region_hdr> macho_region_fh{
            region_path, false, log_macho_region_hdr_magic, &macho_region_hdr_buf, 1};
        macho_region_fh.write(region.bytes);
    }

    if (!m_stream) {
        for (const auto &[tid, buf] : m_log_bufs) {
            const log_thread_hdr thread_hdr{.thread_id = tid, .num_inst = m_thread_num_inst[tid]};
            CompressedFile<log_thread_hdr> thread_fh{m_log_dir_path /
                                                         fmt::format("thread-{:d}.bin", tid),
                                                     false, /* read */
                                                     log_thread_hdr_magic,
                                                     &thread_hdr,
                                                     m_compression_level,
                                                     true /* verbose */};
            thread_fh.write(buf);
        }
    } else {
        for (const auto &[tid, cf] : m_log_streams) {
            cf->header().num_inst = m_thread_num_inst[tid];
        }
    }
}