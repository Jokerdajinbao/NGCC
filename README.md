# NGCC — 简易 C 编译器（NG's C Compiler）

NGCC 是一个用 **C 语言**从零实现的简易 C 编译器，可以把 `.c` 源文件**直接编译成 Windows x64 原生可执行文件（.exe）**，用法类似 gcc。整个编译器是**单个源文件**（约 4300 行 C），没有任何第三方依赖。

**NGCC 已经可以自举（self-hosting）**：`ngcc.exe` 能编译自己的源码 `NGCC.c`，再编译出下一代编译器，产物功能完全等价。

---

## 快速开始

```bat
ngcc HelloWorld.c              → 生成 HelloWorld.exe
HelloWorld.exe                 → 运行，输出 Hello World!
```

命令行智能补全（类似 gcc 的便捷用法）：

```
ngcc e              → 等价于 ngcc e.c
ngcc e e            → 等价于 ngcc e.c e.exe（第二个参数是输出文件名）
ngcc e.c -o out     → 显式指定输出仍可用
```

版本查询：

```
ngcc -v             → NGCC version 1.0.0
ngcc --version      → NGCC version 1.0.0
```

版本号定义在 `NGCC.c` 开头的 `#define NGCC_VERSION "..."`，发新版本时改这一处即可。

---

## 特性

- **单文件编译器**：`NGCC.c` 一个源文件实现全部功能（词法/语法/代码生成/PE 写入）
- **直接生成原生 exe**：手写 PE32+ 写入与导入表，调用 `msvcrt.dll` / `kernel32.dll`
- **不需要链接器**：多文件编译在内部完成符号合并，一步生成可执行文件
- **不需要 CRT 启动代码**：自写 `_start` 入口
- **自举**：能用自己编译自己
- **自动依赖发现**：调用的函数只有声明没有定义时，自动扫描同目录 `.c` 补全
- **本地头文件**：支持 `#include "sub/name.h"`（含子目录、嵌套、include guard）

编译管线：

```
C 源码 → 词法分析 → 语法分析(AST) → x86-64 机器码 → PE32+ 可执行文件写入
```

---

## 支持的 C 子集

**类型**：`int`、`char`、`long` / `long long` / `unsigned`（64 位语义）、`double`（64 位 IEEE-754）、
`float`（真正的 32 位 IEEE-754：4 字节存储、`sizeof(float)=4`、SSE 32 位指令、`1.5f` 字面量后缀、
float 与 double 混合运算提升为 double、变参调用中 float 自动提升 double、`(float)` 转换截断为 32 位）、
指针（任意多级，可存储/传递/下标/解引用/算术）、数组（多维 `int a[2][3]`、尺寸自动推导、
部分初始化自动补零、数组参数、下标读写）、结构体（`struct`/`typedef`/`enum`、成员访问 `.`/`->`、
按值传参、复合字面量 `(Type){...}`）、函数指针

**语句**：块 `{}`（支持块作用域）、`if/else`、`while`、`for`、`switch/case/default`、
`do-while`、三元 `?:`、`return`、`break`、`continue`、表达式语句、声明（可带初始值、
可逗号分隔、局部 `static`）

**运算符**：`+ - * / %`、`< <= > >= == !=`、`&& || !`、`& | ^ ~`、`<< >>`、
一元负号、赋值及全部复合赋值、前缀/后缀 `++ --`、下标、取地址、解引用、
类型转换、`sizeof`（类型/变量/表达式编译期求值）、逗号运算符

**函数**：自定义函数（任意参数个数，x64 寄存器 + 栈传参）、递归、变参函数
（`...` + `va_start/va_end`，配合 `vfprintf`）、`main(argc, argv)`（真实命令行参数）、
函数原型声明（可与定义合并，未定义时报告 undefined reference）

**多文件编译**：`ngcc a.c b.c` 把多个源文件编译成一个 exe——共享函数定义/原型、
全局变量（含 `extern` 声明合并）、typedef、struct 定义、枚举、宏

**预处理**：`#include "local.h"` 递归展开本地头文件（含子目录、嵌套 include 与
include guard，同一头文件每编译单元只展开一次）；`#include <...>` 系统头忽略；
`#define` 对象宏（可嵌套展开，跨头文件/跨文件共享）；`//` 与 `/* */` 注释

**结构体**：支持前向声明（`struct A;`）、自引用/互引用指针、`sizeof(struct A)`
（完整类型才可 sizeof，不完整类型报错）

**字面量**：十进制/十六进制/八进制整数、浮点字面量（含 `1.5`、`1e-10`、`2.5E3`、`1.5f` 等写法）、
字符与字符串转义、UTF-8 BOM 处理

**库函数**（来自 msvcrt.dll）：`printf scanf puts putchar getchar exit malloc free calloc realloc
rand srand time fopen fread fwrite fclose fseek ftell fprintf vfprintf fputc memcpy memset
memcmp strcmp strlen strncmp strchr strrchr getenv __iob_func pow`；
来自 kernel32.dll：`GetCommandLineA ExitProcess`

---

## 限制（有意为之的简化）

- 无 `goto` / `union` / 位域
- 无优化；除零会触发 CPU 异常（与 gcc 行为一致）
- 浮点字面量解析为手写十进制→二进制转换，`printf` 的 `%f` 舍入遵循 msvcrt 行为
- 错误信息为英文

---

## 实现要点

| 阶段 | 说明 |
|------|------|
| 词法分析 | 手工实现，处理注释/预处理指令/宏展开/本地头文件递归展开/转义/CRLF/BOM；浮点字面量手写解析；宏表全局共享 |
| 语法分析 | 递归下降，标准 C 优先级全链；函数原型注册为无 body 的函数，跨编译单元与定义合并 |
| 代码生成 | 栈式 x86-64 指令编码；局部变量放 `[rbp-off]` 槽位；调用点预留 shadow space 并保证 rsp 16 字节对齐；64 位类型走 REX.W；double/float 用 SSE 指令（值在 XMM0，变参调用时同时放进 XMM 与对应整数寄存器以便 msvcrt 的 va_list 读取） |
| 链接 | 多文件共享函数表与全局符号表；`extern` 声明与定义合并；未定义函数/`main` 报告 undefined reference |
| PE 写入 | 直接输出 PE32+（AMD64，console 子系统，ImageBase 0x140000000），手写导入表与 RIP 相对寻址，`.text/.data/.idata` 动态布局 |
| 入口 | 自写 `_start`：对齐栈 → 初始化全局变量 → 解析命令行（`GetCommandLineA`）→ `call main` → `call ExitProcess` |

单个源文件 `NGCC.c`（约 4300 行，无第三方依赖，只用 C 标准库）。

---

## 从源码构建

gcc -O2 -Wall -Wextra -o ngcc.exe NGCC.c

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `ngcc.exe` | 已构建好的编译器（可直接使用） |
| `NGCC.c` | 编译器完整源码（C 语言，单文件） |
| `README.md` | 本文档 |
| `example` | 测试程序 |

> `ngcc.exe` 运行时完全不依赖任何外部工具链。

---

## 作者

- GitHub：https://github.com/Jokerdajinbao
- Bilibili：https://space.bilibili.com/41660208
- 本项目使用 Deepseek Harness agent 工具辅助开发
