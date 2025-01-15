> 该目录中的文件是经过拆分的腾讯会议安装包，用于 Fedora36+ 系统，需要合并后才能安装

```sh
# 合并命令如下
cat wemeet_part_* > TencentMeeting_0300000000_3.19.0.401_x86_64_default.publish.deb
```

```sh
# 切分命令如下
split -b 10M TencentMeeting_0300000000_3.19.0.401_x86_64_default.publish.deb wemeet_part_
```

### deb 拆包使用

```sh
alien -r TencentMeeting_0300000000_2.8.0.1_x86_64.publish.deb #将deb->rpm
rpm2cpio wemeet-2.8.0.1-2.x86_64.rpm |cpio -ivmd #get rpm content
cd ./opt/wemeet
# 运行 tencent meeting
./wemeetapp.sh
```

### 如果提示 wayland 不支持

```sh
vim /etc/gdm/custom.conf #add
WaylandEnable=false
DefaultSession=gnome-xorg.desktop
```

### if the fonts display failure

```sh
QT_FONT_DPI=96 ./opt/wemeet/wemeetapp.sh
```
