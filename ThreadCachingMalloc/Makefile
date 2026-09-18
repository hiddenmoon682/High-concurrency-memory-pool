# 产物统一输出到 bin/（该目录被 .gitignore 忽略，可随时删除重建）
bin/tcmalloc:tests/BenchMark.cc | bin
	g++ -o $@ $^ -std=c++11

# 用 mkdir 而不是 "mkdir -p"：Windows 下 mingw32-make 走 cmd.exe，
# 会把 -p 当成目录名建出一个名为 "-p" 的垃圾目录
bin:
	mkdir bin

.PHONY:clean

clean:
	rm -rf bin
