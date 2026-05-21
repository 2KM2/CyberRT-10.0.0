# install.py 学习笔记

`install.py` 是这个 CyberRT 工程的第三方依赖安装脚本。它不是单纯安装 Python 包，而是负责下载、编译、安装一批 C/C++ 第三方库。

常用命令：

```bash
python3 install.py --proxy gitee
```

执行后，源码通常放在：

```text
third_party/
```

编译安装产物通常放在：

```text
install/
```

后续编译 CyberRT 时，会从 `install/include`、`install/lib`、`install/lib/cmake` 等目录查找这些依赖。

## 参数说明

入口参数定义在 `install.py` 末尾的 `parse_config()`。

```bash
python3 install.py \
  --platform x86_64 \
  --install_prefix install \
  --proxy gitee
```

参数含义：

- `--platform`：目标平台，默认是 `platform.machine()`，在 x86_64 机器上通常就是 `x86_64`。
- `--install_prefix`：第三方库的安装目录，默认是仓库根目录下的 `install/`。
- `--proxy`：第三方源码下载来源。`gitee` 使用 Gitee 镜像；其他值默认使用 GitHub。

## 主要类结构

脚本里主要有两个类。

### ThirdParty

`ThirdParty` 保存第三方库名称和仓库地址的映射。

例如：

```python
"protobuf": {
    "github": "https://github.com/protocolbuffers/protobuf.git",
    "gitee": "https://gitee.com/minhanghuang/protobuf.git",
}
```

当你使用：

```bash
python3 install.py --proxy gitee
```

脚本就会优先从 Gitee 地址 clone 第三方库。

### Install

`Install` 是真正执行安装流程的类。它保存几个关键路径：

```text
_current_path     当前 CyberRT 仓库根目录
_dowload_path     third_party 目录，注意源码里拼写是 dowload
_install_prefix   install 目录，或者 --install_prefix 指定的目录
_proxy            github 或 gitee
```

## 执行总流程

总流程在 `Install.start()` 中：

```python
self._install_gcc()
self._install_cmake()
self._install_setup()
self._install_tinyxml2()
self._install_dds2()
self._install_nlohmann_json()
self._install_proj()
self._install_gfamily()
self._install_gperftools()
self._unpack_bvar()
```

可以理解为按顺序准备编译器、CMake 和 CyberRT 需要的第三方库。

## 每一步做什么

### 1. _install_gcc()

作用：检查 GCC 版本。

它会执行：

```bash
gcc --version
```

如果版本满足要求，就跳过安装。否则会尝试执行：

```bash
sudo apt update
sudo apt install -y software-properties-common
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt install -y gcc-9 g++-9
```

在你的 Ubuntu 24 容器里，如果 GCC 已经够新，这一步会直接跳过。

### 2. _install_cmake()

作用：检查 CMake 版本。

它会执行：

```bash
cmake --version
```

如果版本大于 3.20，就跳过安装。否则会下载 CMake 3.22.0 二进制包，并复制到 `/usr/local`。

### 3. _install_setup()

作用：安装一个名为 `setup` 的辅助项目。

流程大致是：

```bash
git clone <setup repo> third_party/setup --depth=1
cd third_party/setup
mkdir -p build
cd build
cmake -DCMAKE_INSTALL_PREFIX=<install_prefix> ..
make install -j$(($(nproc) - 1))
```

这个项目通常会往 `install/` 里生成一些环境配置文件，比如 `setup.bash` 或 `setup.zsh`。

### 4. _install_tinyxml2()

作用：编译安装 `tinyxml2`。

指定分支/版本：

```text
tinyxml2 8.0.0
```

安装为动态库：

```bash
cmake -DCMAKE_INSTALL_PREFIX=<install_prefix> -DBUILD_SHARED_LIBS=ON ..
```

### 5. _install_dds2()

作用：编译安装 Fast-DDS 相关依赖。

包含：

```text
asio
foonathan_memory_vendor
Fast-CDR
Fast-DDS
```

版本大致是：

```text
asio                  asio-1-18-1
foonathan_memory      v1.3.1
Fast-CDR              v2.2.2
Fast-DDS              v2.14.3
```

其中 `Fast-CDR` 会打补丁：

```bash
patch -p1 < scripts/Fast-CDR_v2.2.2.patch
```

### 6. _install_nlohmann_json()

作用：安装 `nlohmann_json`。

这是一个常用的 C++ JSON 库。脚本会关闭测试：

```bash
cmake -DCMAKE_INSTALL_PREFIX=<install_prefix> -DJSON_BuildTests=OFF ..
```

### 7. _install_proj()

作用：安装 `PROJ`，主要用于坐标转换/地图相关能力。

版本：

```text
PROJ 7.1.0
```

会打补丁：

```bash
patch -p1 -N < scripts/PROJ-7.1.0.patch
```

### 8. _install_gfamily()

作用：安装 Google 系列依赖和 protobuf。

包含：

```text
gflags      v2.2.0
glog        v0.4.0
googletest  release-1.10.0
protobuf    v3.14.0
```

这里的 protobuf 是 C++ protobuf，从源码编译安装到 `install/`。

注意：这和 Dockerfile 里的：

```bash
python3 -m pip install protobuf==3.14.0
```

不是同一件事。

- Dockerfile 里的 protobuf 是 Python 包。
- `install.py` 编译的 protobuf 是 C++ 库和 `protoc` 等工具。

CyberRT 两边都可能需要，所以版本保持一致是合理的。

### 9. _install_gperftools()

作用：安装 `gperftools`，里面包含 `tcmalloc`。

版本：

```text
gperftools-2.8
```

安装后，如果发现：

```text
install/lib/libtcmalloc_minimal.so
```

脚本会尝试往 `install/setup.bash` 和 `install/setup.zsh` 追加：

```bash
export LD_PRELOAD=<install>/lib/libtcmalloc_minimal.so:$LD_PRELOAD
```

这样后续运行程序时可以预加载 tcmalloc。

### 10. _unpack_bvar()

作用：下载并解包 `bvar` 的 deb 包。

x86_64 下文件名是：

```text
bvar_9.0.0-rc-r2_amd64.deb
```

脚本不会用 `apt install` 安装这个 deb，而是用：

```bash
dpkg -x bvar.deb third_party/bvar
cp -r third_party/bvar/usr/local/* install/
```

也就是说，它只是把 deb 包里的 `/usr/local` 内容解压复制到 `install/`。

## 产物目录怎么看

安装完成后，可以重点看这些目录：

```bash
ls install
ls install/include
ls install/lib
ls install/lib/cmake
```

常见内容：

```text
install/include/     第三方库头文件
install/lib/         第三方动态库/静态库
install/bin/         可能包含 protoc 等工具
install/lib/cmake/   CMake package 配置
install/setup.bash   环境变量脚本
install/setup.zsh    环境变量脚本
```

如果后面编译 CyberRT 找不到库，通常要先检查 `install/` 下面是否真的有对应文件。

## 常见问题

### sudo not found

现象：

```text
/bin/sh: 1: sudo: not found
```

原因：容器里没有安装 `sudo`，但 `install.py` 调用了 `sudo apt update`。

解决：

```bash
apt update
apt install -y sudo
```

### CMAKE_MAKE_PROGRAM is not set

现象：

```text
CMake was unable to find a build program corresponding to "Unix Makefiles".
CMAKE_MAKE_PROGRAM is not set.
```

原因：缺少 `make`。

解决：

```bash
apt update
apt install -y make
```

然后清理失败的 build 目录：

```bash
rm -rf third_party/setup/build
python3 install.py --proxy gitee
```

### repo exists 后仍然失败

`_clone_repo()` 的逻辑是：如果 `third_party/<repo>` 已存在，就不再重新 clone。

所以如果某个库源码已经 clone 下来，但 build 失败，下次运行会跳过 clone，继续使用旧目录。

常用处理方式：

```bash
rm -rf third_party/<repo>/build
python3 install.py --proxy gitee
```

如果源码本身坏了，可以删整个库：

```bash
rm -rf third_party/<repo>
python3 install.py --proxy gitee
```

## 学习建议

读这个脚本时，可以先抓住三个核心函数：

```python
start()
_clone_repo()
_cmd()
```

- `start()` 决定安装顺序。
- `_clone_repo()` 决定源码从哪里下载、放到哪里。
- `_cmd()` 负责执行 shell 命令，失败时抛异常并终止脚本。

再去看每个 `_install_xxx()`，就比较容易理解了。
