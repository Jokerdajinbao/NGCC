# NGCC
NGCC — 简易 C 编译器（NG's C Compiler）  NGCC 是一个用 **C 语言**从零实现的简易 C 编译器，可以把 `.c` 源文件直接编译成 Windows x64 原生可执行文件（.exe），用法类似 gcc。整个编译器没有任何第三方依赖。  NGCC 已经可以自举（self-hosting）能编译自己的源码 `NGCC.c`，再编译出下一代编译器，产物功能完全等价。
