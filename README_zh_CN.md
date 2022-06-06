# TON

选择文档语言: [英文](../README.md) | 简体中文 | [其他语言...](./README_zh_CN.md)   


主TON monorepo，包含node/validator、lite-client、tonlib、FunC编译器等代码。

## 更新流程：

* **master 分支** - 主网在这个稳定的分支上运行。

    只有紧急更新、紧急更新或不影响主代码库（GitHub 工作流/docker 图像/文档）的更新直接提交到此分支。

* **testnet 分支** - testnet 正在此分支上运行。该分支包含一组新的更新。测试完成后，将 testnet 分支合并到 master 分支，然后将一组新的更新添加到 testnet 分支。

* **backlog** - 其他可以在下一次迭代中进入测试网分支的分支。

通常，对您的拉取请求的响应将表明它属于哪个部分。


## “软”拉取请求规则

* 你不能合并你自己的 PR，至少一个人应该审查 PR 并合并它（4 眼规则）
* 在考虑合并之前，您应确保您的 PR 的工作流程已完全完成

## 工作流责任
如果 CI 工作流程失败不是因为您的更改而是工作流程问题，请尝试自行修复或通过 Telegram Messenger 联系下列人员之一：

* **C/C++ CI (ccpp-linux.yml)**：待定
* **C/C++ CI Win64 编译（ccpp-win64.yml）**：待定
