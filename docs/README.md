---
AIGC:
  ContentProducer: '001191110102MAD55U9H0F10002'
  ContentPropagator: '001191110102MAD55U9H0F10002'
  Label: '1'
  ProduceID: '8e773950-51d5-47c6-aacd-18443885b2e0'
  PropagateID: '8e773950-51d5-47c6-aacd-18443885b2e0'
  ReservedCode1: '8164c502-06b5-4074-b52b-98d1d289f26f'
  ReservedCode2: '8164c502-06b5-4074-b52b-98d1d289f26f'
---

# Cultures Gold 1 汉化扩展（CulturesGameExtend_Cultures_I）

《Cultures Gold 1》（德语版）的简体中文汉化项目归档仓库，整合了 DLL 扩展工程（CulturesGameExtend）与翻译资产，保留完整 git 历史。

## 项目简介

- 通过 `dinput8.dll` 代理注入方式加载扩展 DLL，实现游戏内文本的简体中文化。
- 扩展 DLL 采用 Feature 自注册框架，翻译字典为 `plugins/translation.tsv`（KEY→译文）。
- 关卡文本以 `update/` 目录覆盖更新（部分文件为 UTF-8，关卡 ini 为 cp1252，切勿混写）。

## 目录结构

```
CulturesGameExtend_Cultures_I/
├── CulturesGameExtend/   # DLL 扩展工程源码（Core / Features / dllmain.cpp）
├── Launcher/             # 启动器源码（Launcher.cpp + vcxproj）
├── plugins/              # 运行期插件配置与翻译字典
│   ├── translation.tsv   # 翻译字典（KEY→简体中文）
│   └── config/           # 全局配置（CulturesGameExtend_Global.ini）
├── update/               # 文本资产（Data / Data_m / data_v，含 .txt/.ini/.tab/.sal/.fnt）
├── translation-assets/   # 翻译工作台与归档分析产物（最小集）
│   └── workspace/        # archive 表、translation_base、topology、summary
├── localization_notes/   # 侦察报告（格式/编码/部署研究）
├── docs/                 # 项目文档（本文件）
└── Cultures1Extend.sln   # 解决方案（引用 DLL 工程与 Launcher）
```

## 构建

1. 使用 Visual Studio 2017（v141 工具集，x86 平台）打开 `Cultures1Extend.sln`。
2. 编译 `CulturesGameExtend` 工程，生成扩展 DLL。
3. 编译 `Launcher` 工程，生成启动器。

## 部署

- 将生成的扩展 DLL 与 `dinput8.dll` 代理放入游戏目录（`Cultures.exe` 所在目录）。
- 将 `plugins/`（translation.tsv 与 config）放入游戏目录。
- 将 `update/` 内容复制到游戏目录（覆盖同名文件；地图/模型等二进制不在此仓库）。
- 游戏测试在本地进行，不随仓库提供游戏本体。

## 历史说明

本仓库由两个来源经 git subtree 合并而成：
- `CulturesGameExtend`（DLL 工程，4 个提交）
- `workspace`（翻译工作区，2 个提交）

请勿直接 push 到外部，上传 GitHub 需先征得用户确认。