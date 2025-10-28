#!/usr/bin/python
import os
import shutil
import re

import subprocess
import sys
import argparse
import threading
import multiprocessing

llvm_install_path = "/home/yl/Application/bin"

CC = llvm_install_path + "clang"
DIS = llvm_install_path + "llvm-dis"
OPT = llvm_install_path + "opt"
Canary = "/home/yl/Documents/Dailycode/memory_leak/canary/build/bin/canary"

ENV = os.environ.copy()

ENV.update({
    "CC": CC,
    "LLD": llvm_install_path + "lld",
    "CFLAGS": "-flto=thin -fuse-ld=lld",
    "RUSTFLAGS": "--emit=llvm-bc -Clinker=" + CC +" -Clink-arg=-fuse-ld=lld",
    "AR": llvm_install_path + "llvm-ar",
})

parser = argparse.ArgumentParser()
parser.add_argument("--c")
parser.add_argument("--r")
parser.add_argument("--d")
parser.add_argument("--auto")
parser.add_argument("--s")
arg = parser.parse_args()


def make():
    return 
    subprocess.run(
        ["make", "-j14"],
        cwd="/home/rustcrate/canary/build",

    )


def testC(inputfile: str, c_source_function:str):

    make()
    print(inputfile)
    if not len(inputfile):
        return
    final_target = (
        inputfile[: inputfile.rfind(".")]
        if not inputfile.endswith(".bc") and not inputfile.endswith(".o")
        else inputfile
    )
    if inputfile.endswith(".c"):
        subprocess.run([CC, inputfile, "-emit-llvm", "-c", "-o", f"{final_target}.bc"])
        final_target += ".bc"

    passBc(final_target);
    result = subprocess.run(
        [Canary, final_target, "--print-c-source-functions", c_source_function,  "--alloc"],
    )
    return result.returncode == 0


def testR(inputfile: str, sourcefile: str):
    make()
    passBc(inputfile);
    print(f"Analysis{inputfile} with sourcefile {sourcefile}")
    if not len(inputfile):
        return
    
    file_out = open(f"{inputfile.split('/')[0]}/{inputfile.split('/')[-1]}-output.log", "w+")
    test_r_result = subprocess.run([Canary, inputfile, "-c-source-functions", sourcefile], stdout=file_out, stderr=file_out)
    file_out.close()
    subprocess.run([DIS, inputfile])
    return test_r_result.returncode ==0


def passBc(inputfile: str):
    subprocess.run(
        [OPT, "-passes=loweratomic", inputfile, "-o", inputfile],
    )
    subprocess.run(
        [OPT, "-passes=lowerinvoke", inputfile, "-o", inputfile],
    )
    subprocess.run(
        [OPT, "-passes=mem2reg", inputfile, "-o", inputfile],
    )
    subprocess.run(
        [OPT, "-passes=sccp", inputfile, "-o", inputfile],
    )
    subprocess.run(
        [OPT, "-passes=loop-simplify", inputfile, "-o", inputfile],
    )
    subprocess.run(
        [OPT, "-passes=simplifycfg", inputfile, "-o", inputfile],
    )
    return


def filter_llvm_files(directory):
    print(directory)
    deps = f"{directory}/target/debug/deps/"
    build = f"{directory}/target/debug/build/"
    llvm_files = []
    for root, dirs, files in os.walk(deps):
        for file in files:
            file_path = os.path.join(root, file)
            result = subprocess.run(["file", "-b", file_path], stdout=subprocess.PIPE)
            file_type = result.stdout
            if file_type == b"LLVM IR bitcode\n" and ( file.endswith('.bc')):
                print(file_path)
                llvm_files.append(file_path)
    for root, dirs, files in os.walk(build):
        for file in files:
            file_path = os.path.join(root, file)
            result = subprocess.run(["file", "-b", file_path], stdout=subprocess.PIPE)
            file_type = result.stdout
            if file_type == b"LLVM IR bitcode\n" and ( file.endswith('.o')):
                print(file_path)
                llvm_files.append(file_path)
    return llvm_files


def testDir(inputfile: str):
    inputfile = inputfile.strip("./")
    c_file = f"{inputfile}/link-obj.o" 
    subprocess.run(["rm", c_file], env=ENV)
    cargo_clean = subprocess.run(["cargo", "clean"], env=ENV, cwd=inputfile, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if cargo_clean.returncode != 0:
        print("cargo clean failed")
        return
    cargo_build = subprocess.run(["cargo", "build"], env=ENV, cwd=inputfile, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if cargo_build.returncode != 0:
        print(cargo_build.stderr.decode())
        # cargo_clean = subprocess.run(["cargo", "clean"], env=ENV, cwd=inputfile, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        print("cargo build failed")
        return
    llvm_files = filter_llvm_files(inputfile)

    c_files = [ i for i in llvm_files if i.endswith(".o") ]
    r_files = [ i for i in llvm_files if i.endswith(".bc") ]
    if len(c_files) > 1:
        print("generate link-obj")
        for i in c_files:
            subprocess.run([DIS, i], env=ENV)
        link_res = subprocess.run(["llvm-link", "-o", c_file] + c_files, env=ENV, stdout=subprocess.PIPE, stderr=subprocess.PIPE )
        print(link_res.returncode, link_res.stdout, link_res.stderr)
    else:
        c_file = c_files[0]
    c_source_function = f'{inputfile}/source_functions.log'
    c_res = testC(c_file, c_source_function)
    print(c_res)
    if c_res:
        r_res = False
        for r_file in r_files:
            r_res = testR(r_file, c_source_function) or r_res
        c_res = c_res and r_res
    # if len(c_files) > 1: 
    #     subprocess.run(["rm", c_file], env=ENV)
    # cargo_clean = subprocess.run(["cargo", "clean"], env=ENV, cwd=inputfile, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return c_res


def test_cargo_build(inputfile):
    success = True;
    cargo_build = subprocess.run(["cargo", "build"], env=ENV, cwd=inputfile)
    if cargo_build.returncode != 0:
        print(inputfile, "is failed to be built\n", end="")
        success = False;
    if not success:
        cargo_clean = subprocess.run(["cargo", "clean"], env=ENV, cwd=inputfile)
    return success 


def is_c_or_cpp_file(file_name):
    c_cpp_extensions = ['.c', '.cpp', '.cc']
    file_ext = os.path.splitext(file_name)[1]
    return file_ext.lower() in c_cpp_extensions

def has_c_cpp_files(directory):
    for root, dirs, files in os.walk(directory):
        for file in files:
            if is_c_or_cpp_file(file):
                return True;
    return False

def is_sys_wrapper(directory):
    return 'sys' in directory

def remove_directories_without_c_or_cpp_file(root_directory):
    for entry in os.scandir(root_directory):
        if entry.is_dir() and not entry.is_symlink():
            if not has_c_cpp_files(entry.path) :
                print(f"Removing directory: {entry.path}")
                shutil.rmtree(entry.path)

def remove_build_failed_directories(entry):
    if not test_cargo_build(entry) :
        print(f"Removing directory: {entry}")
       # shutil.rmtree(entry)
        return entry
    return None

def mutlithread_delete_build_failed_directories(root_directory):
    entry_list = [ entry.path for entry in os.scandir(root_directory) if entry.is_dir]
    print(entry_list)
    pool = multiprocessing.Pool()
    result_list = pool.starmap(testDir, [[i] for i in entry_list])
    for i in result_list:
        print(i)
     

#def serial_check_directories(root_directory):
#    entry_list = [entry.path for entry in os.scandir(root_directory)]
#    print(entry_list)
#    result_list =  []
        

def work_for_all_directories(func, root_dir):
    for dirpath, dirnames, filenames in os.walk(root_dir):
        for dirname in dirnames:
            func(dirname)
            # subprocess.run(["cargo", "clean"], env=ENV, cwd=dirname)


if __name__ == "__main__":
    # root_directory = sys.argv[1]  # Specify the root directory where you want to perform the renaming
    # serial_check_directories(root_directory)
    # work_for_all_directories(test_cargo_build, ".") 
    if arg.c:
        testC(arg.c, arg.s)
    if arg.r:
        testR(arg.r, arg.s)
    if arg.d:
        testDir(arg.d)
    if arg.auto:
        mutlithread_delete_build_failed_directories(arg.auto)
    
