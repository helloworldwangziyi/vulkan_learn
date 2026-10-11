# LazyVim 使用指南（本机环境）

从 VS Code 迁移到 Neovim + LazyVim 的完整记录：装了什么、为什么这么配、日常怎么用、出问题怎么查。

- Neovim 0.12.5：`C:\Users\Administrator\AppData\Local\Programs\Neovim`
- 配置目录：`~/AppData/Local/nvim/`（旧的手写配置备份在 `~/AppData/Local/nvim-backup/`）
- 本文所有"配置在 xxx"均指 nvim 配置目录下的文件

---

## 一、本机环境清单

| 组件 | 版本/位置 | 作用 |
|---|---|---|
| LazyVim + 37 插件 | `nvim-data/lazy/` | 发行版 + 插件（lazy.nvim 管理） |
| clangd | 23.1.0，`nvim-data/mason/` | C++ 补全/跳转/实时诊断 |
| clang-format | 23.1.3，同上 | 格式化（`<空格>cf`） |
| cmake-language-server | 0.1.11，同上 | CMakeLists.txt 支持 |
| treesitter parser | 27 个，`nvim-data/site/parser/` | 语法高亮（含 cpp/glsl/cmake） |
| tree-sitter CLI | mason 安装 | parser 编译驱动 |
| zig + ccwrap | `C:\Users\Administrator\Files\zig\` | parser 的 C 编译器链（见坑 #3~5） |
| Qt MinGW gcc 13.1 | `C:\Users\Administrator\Files\qt\Tools\mingw1310_64\` | 备用 C 编译器（ccwrap 就是它编的） |
| ripgrep / fd | winget 安装 | 全文搜索 / 文件查找 |
| 字体 | CaskaydiaCove **NFM**（全机器安装） | Nerd Font 图标 |
| 代理 | clash `127.0.0.1:7890` | nvim 内配在 `options.lua`（GitHub 直连不通） |
| 终端 shell | Windows PowerShell **5.1**（powershell.exe，没装 pwsh 7） | nvim 里所有 `:terminal` / `:!` / `<空格>ft` / `<空格>cb` 都走它 |

## 二、踩坑记录（配置为什么这么奇葩）

按时间顺序，每个坑都是本机实测：

1. **GitHub 直连不通**。curl/git 直接访问会 SSL 握手失败或超时。→ `options.lua` 里给 nvim 进程配 `http_proxy/https_proxy`（clash 没开时把那几行注释掉）。
2. **Mason 装 clangd 卡死/报锁**。`Lockfile exists ... --force` 是上一个没退干净的 nvim 留的锁。→ 确认没有 nvim 残留进程后重跑 `MasonInstall --force clangd`。
3. **MinGW 的 ld 写不了 `\\?\` 路径**。tree-sitter CLI 输出路径带 Windows verbatim 前缀，`ld.exe` 报 `Invalid argument`（已用 hello world 实测复现）。→ 换 zig cc。
4. **新版 zig 不认 `-msvc` 目标**。CLI 强塞 `--target=x86_64-pc-windows-msvc`，zig 0.17 报 `UnknownOperatingSystem`（该 ABI 后缀已废弃）。→ 装 **zig 0.14.1**。
5. **zig 仍被塞 `--target`**。→ 写了 `ccwrap.exe`（C 写的 30 行包装器，源码 `Files/zig/ccwrap.c`）：剥掉所有 `-target/--target` 参数再转发给 `zig cc`。nvim 里 `CC` 环境变量指向它。
6. **字体名字坑**。Nerd Fonts 的 Mono 变体内部族名是 `CaskaydiaCove NFM`（不是 `NF Mono`），Windows Terminal 按名找字体，名字错一个字母就"找不到字体"。
7. **火绒会静默杀编译器**。MSYS mingw64 的 `cc.exe` 被无声杀掉（exit 1 无输出），CMake 探测到它就 configure 失败。→ `CMakePresets.json` 显式锁定 Qt 的 `gcc.exe`/`g++.exe`。
8. **`cmake --preset` 只认当前目录**。在 `lessons/` 子目录里跑会报 "Could not read presets"。→ 自定义键位先从当前文件向上找 `CMakePresets.json` 定位项目根，再 `cd` 过去跑。
9. **终端 shell 换成了 PowerShell 5.1，cmd 语法全作废**。`cd /d` → `Set-Location`，`&&` → `;`（5.1 没有 `&&`，pwsh 7 才有）。配置走 LazyVim 助手 `LazyVim.terminal.setup("powershell")`：装了 pwsh 会自动升级用它，没装就退回 powershell.exe 并配好 quoting/UTF-8。但它默认那串 shellcmdflag 是按 pwsh 7 写的（`$PSStyle`、`Remove-Alias` 都是 7 才有的），5.1 下每次开终端会先刷两条红色报错（不影响执行，纯碍眼）→ `options.lua` 里手动覆盖成 5.1 兼容版；哪天装了 pwsh 7，把那行覆盖删掉即可。

## 三、日常使用

### 启动

```bash
cd C:/Users/Administrator/Desktop/VulkanLearn
nvim .
```

### 模式（80% 的"按键没反应"都是模式不对）

| 左下角显示 | 含义 | 怎么进/出 |
|---|---|---|
| `-- NORMAL --`（不显示） | 普通模式，快捷键只在它下生效 | `Esc` 或 `jk` 进入 |
| `-- INSERT --` | 插入模式，打字进缓冲区 | `i`/`a`/`o` 进入 |
| `-- TERMINAL --` | 终端输入模式 | 点进终端窗口进入；`jk` 或 `Ctrl+\ Ctrl+n` 退出 |

**中文输入法要在英文模式再按快捷键**，拼音合成状态会吞键。

### 键位速查（空格是 Leader，按了空格会弹菜单）

文件与搜索：

| 键位 | 作用 |
|---|---|
| `<空格>ff` | 模糊找文件 |
| `<空格>/` | 全文搜索（ripgrep） |
| `<空格>e` | 文件树 |
| `<空格>,` / `<空格>.` | 切换缓冲区 |
| `Shift+h` / `Shift+l` | 左右切文件标签 |

代码（LSP = clangd）：

| 键位 | 作用 |
|---|---|
| `gd` / `gr` | 跳定义 / 找引用（能跳进 Vulkan/GLFW 头文件） |
| `K` | 查看符号文档 |
| `<空格>ca` | 代码操作（Quick Fix） |
| `<空格>cf` | 格式化（clang-format） |

窗口与终端：

| 键位 | 作用 |
|---|---|
| `Ctrl+h/j/k/l` | 窗口间跳转 |
| `<空格>ft` | 右侧终端（PowerShell，和编辑区并排） |
| `<空格>cb` | **构建当前课程**（CMake，见下） |
| `<空格>cB` | 构建当前课程（debug preset） |

### 构建（自定义键位的逻辑）

`<空格>cb` 做的事：从当前文件向上找项目根 → 右侧开 60 列竖分屏终端（PowerShell）→ 按项目类型执行：

| 项目类型 | 执行的命令 |
|---|---|
| 有 `CMakePresets.json` | `cmake --build --preset default` |
| 只有 `CMakeLists.txt` | `cmake -B build -S . && cmake --build build` |
| 当前打开 `.cpp` | 上面两条都附加 `--target 文件名`，只编这一个 |
| 都没有 | 弹提示，不瞎跑 |

终端跑完输出留在窗口里随时回看，构建期间不阻塞编辑。

### 手动编译（万能兜底）

`<空格>ft` 开终端，手敲任意命令；终端里按 `↑` 翻历史。

```powershell
cmake -B build -S . ; cmake --build build   # 裸 CMake 项目的两步走（5.1 没有 &&，用 ; 连接）
make                                        # Makefile 项目
g++ main.cpp -o main                        # 单文件
```

**configure 一次、build 无数次的模型**：第一句生成构建系统（每个项目 clone 下来做一次，或改了 CMakeLists.txt 后重做）；第二句增量编译，日常只敲它。

### 关闭终端窗口

1. 终端里 `exit` 回车（最干净，进程退出窗口自动关）
2. `jk` 退出终端模式 → `:q`
3. 构建跑一半 `:q` 会连进程一起杀掉

## 四、换项目怎么办

```text
有 CMakePresets.json？──是──→ <空格>cb 直接用
        │否
        └─ 有 CMakeLists.txt？──是──→ <空格>cb 自动走 cmake -B build 流程
                    │否
                    └─ 有 Makefile/Cargo.toml？──是──→ <空格>ft 开终端跑 make/cargo
                                │否
                                └─→ 给项目定制键位：编辑 keymaps.lua 照抄改命令
```

## 五、配置文件导览（`~/AppData/Local/nvim/lua/`）

| 文件 | 内容 |
|---|---|
| `config/lazy.lua` | 插件管理入口；imports 里启用了 `lang.clangd`、`lang.cmake` 两个官方扩展 |
| `config/options.lua` | ① MinGW/zig 加入 PATH（仅 nvim 进程内）② 代理环境变量 ③ GBK 文件编码兼容 ④ Nerd Font 开关 ⑤ PowerShell 终端（5.1 兼容的 shellcmdflag，见坑 #9） |
| `config/keymaps.lua` | ① `jk` 退出插入/终端模式 ② `project_info()` 向上探测项目根 ③ `build_current()` 构建逻辑（PowerShell 语法）④ `<leader>cb/cB` |
| `plugins/cpp.lua` | treesitter 补装 cpp/glsl/cmake 等 parser；vim-glsl 插件识别 `.vert/.frag` 文件类型 |
| `plugins/terminal.lua` | snacks 终端从浮动窗口改到右侧竖分屏（`<空格>ft` / `<C-/>` 生效） |

改完键位/选项立即生效的命令：

```vim
:luafile ~/AppData/Local/nvim/lua/config/keymaps.lua
```

## 六、出问题排查

```vim
:checkhealth          " 全面自检（treesitter/mason/lsp/终端）
:Lazy                 " 插件管理界面（I = 安装详情，U = 更新）
:Mason                " LSP/工具管理（看 clangd 装没装上）
:messages             " 启动报错都躺在这里
```

常见症状对号入座：

| 症状 | 大概率原因 |
|---|---|
| 打开 cpp 没有补全 | 没装 clangd → `:Mason` 里装；或 nvim 没重启 |
| 高亮全是白字 | parser 没编 → `:TSInstall cpp`（自动走 ccwrap 链） |
| Mason 装东西失败 | clash 没开 → 开代理，或注释 options.lua 里代理行再试 |
| 图标显示成方块 | Windows Terminal 没重启，或字体名不对（必须是 `CaskaydiaCove NFM`） |
| 按键没反应 | 不在普通模式 / 中文输入法在合成 / 改了配置没 `:luafile` 或重启 |
| cmake 报 Could not read presets | 在子目录里手动跑了 cmake → 让 `<空格>cb` 处理，它会自动 cd 到项目根 |

## 七、和 VS Code 的分工

- **Neovim**：日常写代码（轻量，无扩展宿主空转，风扇安静）
- **VS Code**：仅当调试器用（F5 + gdb，配置在 `.vscode/launch.json`，先 `cmake --build --preset debug`）
- VS Code 里的 `vscode-neovim` 扩展已无用，可在扩展面板禁用
