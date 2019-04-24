#!/usr/bin/env python3
#
# This file is part of SEL - SecureEpiLink
# Joins multiple csv files on a list of fields. Per file, a tag is read from the
# filename, assuming the filename is of the form {tag}_\w*.csv. This tag is
# then appended to all output fields after a dot, i.e., field {x} of file
# {tag}_...csv becomes {x}.{tag}.

from itertools import groupby
import sys, csv, argparse, os.path, glob, re
from agate import Table, Formula, Number
from decimal import Decimal
from functools import reduce

def main():
    args = parse_args()

    file_lists = get_file_lists(args.basenames)
    for suf, flist in file_lists.items():
        joined = join_csv_files(flist, args)
        table = filter_table(args, joined)
        write_csv_table(args.output, suf, table)
    # read all files
    #   reads tag from file name
    #   reads file as csv
    #   appends .{tag} to each column in out_fields
    # join read tables on join_fields
    # write result with out_fileds + join_fields to new CSV file

def parse_args():
    def split_comma(s): return s.split(',')

    parser = argparse.ArgumentParser(description=("Joins several CSV files with "
            "same columns."))
    parser.add_argument("basenames", metavar="basename", nargs="+",
            help=("Input CSV file basenames. If, for each file, there exist "
                "several files {file}_{suffix}.csv, joins will be created for "
                "each suffix"))
    parser.add_argument("-o", "--output", help="Output CSV file basename.")
    parser.add_argument("-d", "--delimiter", default=",",
            help="Delimiter of input CSVs (default: ',')")
    parser.add_argument("-j", "--join", default="parameters.dbSize",
            help="Field to join on.")
    parser.add_argument("-f", "--fields", type=split_comma,
            default=["setupTime.mean", "onlineTime.mean"],
            help=("Comma separated list of fields to include in output. "
                  "Fields will be included from each file, with the file basename"
                  " appended after a dot."))
    parser.add_argument("--add-comm", action='store_true',
            help=("Create new column by adding communication columns "
                "communication.{setup,online}Comm{Sent,Recv}"))

    args = parser.parse_args()

    # Interpret \t as tab char
    if args.delimiter == "\\t": args.delimiter = "\t"

    return args

def get_file_lists(basenames):
    # check single file case
    paths = [f'{x}.csv' for x in basenames]
    if all(map(os.path.isfile, paths)):
        return {None: paths}

    suffixes = [
        re.match(f'{basenames[0]}_([a-zA-Z0-9+-]+)\.csv', fpath).group(1)
        for fpath in glob.glob(f'{basenames[0]}_*.csv')
    ]

    return {suf: {base: f'{base}_{suf}.csv' for base in basenames} for suf in suffixes}

def join_csv_files(filelist, args):
    tables = []
    for base, f in filelist.items():
        t = Table.from_csv(f, delimiter=args.delimiter)
        if args.add_comm:
            print("adding comm cols")
            t = t.compute([comm_adder_computation('setup'),
                           comm_adder_computation('online')])

        t = t.rename([x if x in [args.join, 'setupComm', 'onlineComm']
                        else f'{x}.{base}'
                for x in t.column_names])

        tables.append(t)

    return reduce(lambda left, right: left.join(right, args.join), tables)

def comm_adder_computation(phase: str):
    return (f"{phase}Comm", Formula(Number(),
        lambda x: x[f'communication.{phase}CommSent']
                + x[f'communication.{phase}CommRecv']
    ))

def filter_table(args, table):
    cols = [args.join]
    if args.add_comm: cols.extend(["setupComm", "onlineComm"])

    field_selector = lambda fld: any(map(fld.startswith, args.fields))
    cols.extend(filter(field_selector, table.column_names))

    return table.select(cols)

def write_csv_table(outbase, suf, table):
    table.to_csv(f"{outbase}_{suf}.csv")


if __name__ == '__main__': main()
