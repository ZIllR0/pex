clang test.c -c -emit-llvm -o  test.bc -O0 -g

/opt/toolchain/llvm-git/bin/opt \
	-analyze -load=/home/t.zhang2/capchk/build/capchk/libcapchk.so \
	-capchk -stats -ccfv=1 -ccvv=1 \
	-cvf=1\
    -dump-callgraph=1\
    -print-pag=0 \
    -print-all-pts=0 \
    -print-fp=0 \
	test.bc -o /dev/null 2>&1
