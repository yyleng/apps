> 该目录中的文件是经过拆分的钉钉安装包，用于 Fedora36+ 系统，需要合并后才能安装

```sh
# 合并命令如下
cat dingtalk_part_* > dingtalk-1.4.0-20829.fc36.x86_64.rpm
```

```sh
# 切分命令如下
split -b 10M dingtalk-1.4.0-20829.fc36.x86_64.rpm dingtalk_part_
```

## 配置中文输入

### 安装 fcitx

```sh
sudo dnf install fcitx fcitx-configtool fcitx-pinyin
fcitx-configtool
# 1. 点击添加输入法
# 2. 将 Only Show Current Language 复选框关闭掉
# 3. 在搜索框中输入 pinyin
# 4. 选择 PinYin
# 5. 点击确定，然后移动到第一位
```

### 配置 dingtalk 启动文件

> 如果无法输入中文，可以尝试一下设置

```sh
vim /opt/apps/com.alibabainc.dingtalk/files/Elevator.sh
#在开头添加
export GTK_IM_MODULE="fcitx"
export QT_IM_MODULE="fcitx"
export XMODIFIERS="@im=fcitx"

```

### 重启 dingtalk

```sh
ps -ef | grep dingtalk
kill -9 <pid>
# 或点击图表启动
/opt/apps/com.alibabainc.dingtalk/files/Elevator.sh

```
