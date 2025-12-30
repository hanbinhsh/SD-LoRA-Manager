# LoRA Manager (SD Model Viewer)

一个优雅、高（？）性能的本地 Stable Diffusion LoRA 模型管理和图库管理工具。（仅适用于Civit下载或者存在于Civit中的模型，国内需要魔法）
专为解决本地模型混乱、预览图缺失、元数据难以查看而设计。无需启动 WebUI，即可快速浏览、筛选和管理你的 LoRA 库。

<img width="1559" height="1090" alt="3 $WD1YW8KQ$K)V_JX88LYL" src="https://github.com/user-attachments/assets/1cbb2a93-02fa-4bd8-b14b-1a225f95846e" />


## ✨ 主要特性 (Features)

* **🎨 沉浸式 UI 设计**
* **Hero 风格详情页**：采用类似 Steam/游戏库的沉浸式设计，顶部大图展示。
* **动态背景模糊**：根据当前模型封面自动生成高斯模糊背景，并支持平滑的交叉淡入淡出 (Cross-fade) 动画，切换模型丝滑流畅。
* **自适应布局**：支持窗口任意缩放，背景与前景图完美对齐。


* **🚀 高性能与本地化**
* **C++ & Qt 构建**：相比 Electron 或 Python 界面，拥有更低的内存占用和更快的启动速度。
* **多线程扫描**：内置线程池技术，秒级（划掉）加载数千个本地模型文件。（假的）
* **懒加载机制**：画廊视图采用异步加载，拒绝界面卡顿。


* **☁️ 智能元数据获取**
* **自动匹配 Civitai**：通过计算本地文件 SHA256 哈希值，自动从 Civitai API 拉取模型封面、简介、版本信息及触发词。
* **触发词一键复制**：详情页直接展示 Trigger Words，点击即可复制，提高生图效率。


* **📂 强大的管理功能**
* **自定义收藏夹**：支持创建多个收藏夹（如“二次元”、“写实”、“机甲”），右键即可快速归类。
* **多维度筛选**：支持按底模 (SD 1.5, SDXL, Pony 等)、下载量、点赞数、创建日期排序。
* **本地缓存**：元数据和图片自动缓存到本地 JSON，离线状态下也能完整浏览。



## 📸 界面预览 (Screenshots)



### 主页画廊 (Home Gallery)

<img width="1555" height="1090" alt="N{$7`N $ANF3X}69@8}HR" src="https://github.com/user-attachments/assets/8074f089-902c-4256-8124-3fbd6f14c78a" />



### 模型详情页 (Detail View)

<img width="1554" height="1089" alt="QDZH0I_$CJWE@@@5IKYNF31" src="https://github.com/user-attachments/assets/4dfc4017-06d2-4bc3-bc64-d372beb62d5a" />

<img width="1389" height="1091" alt="V(X`341R(MCLU{ }7L~P6X" src="https://github.com/user-attachments/assets/65341130-f515-4170-8629-aa47556673b8" />


### 图库页 (Gallery）

<img width="1553" height="1090" alt="%OFY58 @OF$~QP~QM4 J5R" src="https://github.com/user-attachments/assets/86efbb0c-9fa6-41f5-adb2-b6b7123f9f2e" />

<img width="1553" height="1089" alt="{I~$7%(B %3`P9 PG6CM{9S" src="https://github.com/user-attachments/assets/a36feb06-98d6-498a-a408-1f3d2f88efbc" />


### 设置页 (Settings）

<img width="1627" height="1139" alt="J~4CW7 Z5(95E76RN45IJFW" src="https://github.com/user-attachments/assets/2c0944f3-2102-48b4-a7c2-f3e0a3447350" />


## 🛠️ 安装与运行 (Installation)

### 直接运行 (Windows)

1. 前往 Release 页面下载最新的可执行文件（.exe）。
2. 双击 `SD_LoRA_Manager.exe` 即可运行。
3. 首次运行请在顶部文件选项卡中指定你的 LoRA 模型文件夹路径。

> **注意**：如果提示缺少 DLL，请安装 [VC_Redist.x64](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist)。如果无法联网获取元数据，请确保系统已安装 OpenSSL 库。

### 从源码构建 (Build from Source)

如果你是开发者，想自己编译或修改代码：

**前置要求：**

* Qt 6.2 或更高版本 (推荐使用 MSVC 2019/2022 编译器)（本人构建采用的是6.10.1）
* CMake
* OpenSSL 开发库 (用于 HTTPS 请求)
* 请一定要安装Image Formats插件！！！！！！！！！！！！！！（不然你会为为什么加载不出来图片困扰几个小时，笑）

**步骤：**

1. 克隆仓库：
```bash
git clone https://github.com/hanbinhsh/SD-LoRA-Manager.git

```


2. 使用 Qt Creator 打开 `CMakeLists.txt` 文件。
3. 配置项目（推荐选择 Release 模式以获得最佳性能）。
4. 构建并运行。

## ⚙️ 常见问题 (FAQ)

**Q: 为什么有些模型无法获取封面？**
A: 软件通过计算文件 Hash 去 Civitai 匹配。如果该模型已从 C 站下架，或者你是从其他渠道下载的（Hash 不一致），则无法自动获取。你可以手动将预览图命名为 `模型名.preview.png` 放在同级目录下。

**Q: 启动速度有点慢？**
A: 首次扫描大量模型（>1000个）时需要计算 Hash，这取决于磁盘 IO 速度。后续启动会读取本地缓存，速度会非常快（存疑？）。

**Q: 为什么有这么多BUG？**
A: 你说得对但是软件由本人一人在一天之内完成所有代码（感谢我的AI朋友们）。

## 🤝 贡献 (Contributing)

欢迎提交 Issue 和 Pull Request！
如果你有好的 UI 想法或者功能建议，请随时告诉我。

---

**Developed with ❤️ by IceRinne**
