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
    for (const auto &msg : msgs) {
        pcs.emplace_back(msg.pc);
    }
    return pcs;
}

TraceLog::TraceLog() {
    // nothing to do
}

TraceLog::TraceLog(const std::string &log_path) {
    const auto trace_buf = read_file(log_path);
    const auto trace_hdr = (log_hdr *)trace_buf.data();

    auto region_ptr = (log_region *)((uint8_t *)trace_hdr + sizeof(*trace_hdr));
    m_macho_regions = std::make_unique<MachORegions>(region_ptr, trace_hdr->num_regions);
    for (uint64_t i = 0; i < trace_hdr->num_regions; ++i) {
        region_ptr =
            (log_region *)((uint8_t *)region_ptr + sizeof(*region_ptr) + region_ptr->path_len);
    }

    auto syms_ptr = (log_sym *)region_ptr;
    m_symbols     = std::make_unique<Symbols>(syms_ptr, trace_hdr->num_syms);
    for (uint64_t i = 0; i < trace_hdr->num_syms; ++i) {
        syms_ptr = (log_sym *)((uint8_t *)syms_ptr + sizeof(*syms_ptr) + syms_ptr->name_len +
                               syms_ptr->path_len);
    }

    const auto thread_hdr_end = (log_thread_hdr *)(trace_buf.data() + trace_buf.size());
    auto thread_hdr           = (log_thread_hdr *)syms_ptr;
    while (thread_hdr < thread_hdr_end) {
        std::vector<log_msg_hdr> thread_log;
        const auto thread_log_start = (uint8_t *)thread_hdr + sizeof(*thread_hdr);
        const auto thread_log_end   = thread_log_start + thread_hdr->thread_log_sz;
        auto inst_hdr               = (log_msg_hdr *)thread_log_start;
        const auto inst_hdr_end     = (log_msg_hdr *)thread_log_end;
        while (inst_hdr < inst_hdr_end) {
            thread_log.emplace_back(*inst_hdr);
            inst_hdr = inst_hdr + 1;
        }
        m_parsed_logs.emplace(std::make_pair(thread_hdr->thread_id, thread_log));
        thread_hdr = (log_thread_hdr *)thread_log_end;
    }
}

uint64_t TraceLog::num_inst() const {
    return m_num_inst;
}

size_t TraceLog::num_bytes() const {
    size_t sz = 0;
    for (const auto &thread_log : m_log_bufs) {
        sz += thread_log.second.size();
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
    std::copy((uint8_t *)&msg_hdr, (uint8_t *)&msg_hdr + sizeof(msg_hdr),
              std::back_inserter(m_log_bufs[thread]));
    ++m_num_inst;
}

void TraceLog::write_to_file(const std::string &path, const MachORegions &macho_regions,
                             int compression_level, const Symbols *symbols) {
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

    CompressedFile fh{path, false, compression_level};

    const log_hdr hdr_buf{.magic       = log_hdr_magic,
                          .num_regions = macho_regions.regions().size(),
                          .num_syms    = syms.size()};
    fh.write(hdr_buf);

    for (const auto &region : macho_regions.regions()) {
        log_region region_buf{.base     = region.base,
                              .size     = region.size,
                              .slide    = region.slide,
                              .path_len = region.path.string().size()};
        memcpy(region_buf.uuid, region.uuid, sizeof(region_buf.uuid));
        fh.write(region_buf);
        fh.write(region.path.c_str(), region.path.string().size());
    }

    for (const auto &sym : syms) {
        log_sym sym_buf{.base     = sym.base,
                        .size     = sym.size,
                        .name_len = sym.name.size(),
                        .path_len = sym.path.string().size()};
        fh.write(sym_buf);
        fh.write(sym.name.c_str(), sym.name.size());
        fh.write(sym.path.c_str(), sym.path.string().size());
    }

    for (const auto &thread_buf_pair : m_log_bufs) {
        const auto tid           = thread_buf_pair.first;
        const auto buf           = thread_buf_pair.second;
        const auto thread_log_sz = buf.size() * sizeof(decltype(buf)::value_type);
        const log_thread_hdr thread_hdr{.thread_id = tid, .thread_log_sz = thread_log_sz};
        fh.write(thread_hdr);
        fh.write(buf);
    }
}
