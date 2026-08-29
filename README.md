# 多通道信号采集与监测终端

- 基于纳芯微 **NS800RT7P65D**，Ubuntu 环境开发的多通道信号采集与监测终端。搭载 LVGL 图形界面，通过数字编码器与按键组合完成菜单交互。
- 原厂 OpenOCD 缺少 NS800RT7P65D 烧录算法，本项目补齐芯片烧录算法，提供一键编译烧录脚本。

##  使用流程

```bash
# 加载环境变量
source env.sh

# 编译 + 烧录
source f.sh

# 如果pyocd未识别到芯片，找到install.sh  
bash install.sh

```
##  开发任务(目前)  
- [ ] 完成LVGL界面
- [ ] 加入数字编码器
- [ ] 加入传感器
- [x] 点亮LED🥰
