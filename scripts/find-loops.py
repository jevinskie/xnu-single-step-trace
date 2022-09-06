#!/usr/bin/env python3

import argparse

from SuffixTree.suffixtree import SuffixTree
from tracelog import TraceLog


def real_main(args):
    tl = TraceLog(args.trace_file)
    if args.dump:
        tl.dump()
    # pcs = list(tl.traces.values())[0]
    pcs = tl.pcs_for_image(args.image_name)
    print(f"len(pcs): {len(pcs)}")
    st = SuffixTree(pcs)
    st.build_suffix_tree()
    # print(st)
    st.print_dfs()


def get_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="find-loops")
    parser.add_argument("-t", "--trace-file", required=True, help="input trace file")
    parser.add_argument("-d", "--dump", action="store_true", help="dump trace file")
    parser.add_argument("-n", "--image-name", required=True, help="image name to find loops in")
    return parser


def main():
    real_main(get_arg_parser().parse_args())


if __name__ == "__main__":
    main()
