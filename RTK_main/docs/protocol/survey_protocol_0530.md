# 测量软件控制协议-0530 摘录（GNSS 相关）

来源：用户提供 PDF《测量软件控制协议-0530》（2026-11-12）；pdftotext 原文摘录，表格排版有损，以 PDF 为准。PDF 后续可能小改。

```text
                                接收机控制协议
一、说明
  本协议适用于分体式接收机与 APP、测量软件，主要目的是实现通过蓝牙对接收机进
行参数配置及状态显示。
二、编码
  数据协议主要采用二进制编码方式，部分命令信息中的参数，如版本号、APN、用户名
等参数以及包含的 NMEA-0183 报文使用 ASCII 值。
三、格式
  接收机与 APP 软件、测量软件之间的数据交互主要分为三类：
  1、APP、测量软件查询接收机参数，接收机响应返回相关信息；
  2、APP、测量软件配置接收机参数，接收机进行设置，并返回设置是否成功；
  3、APP、测量软件与接收机某一方校验不通过或参数解析不匹配，返回相关错误码给
对方；
  所有数据信息都按以下方式封装：
   Message Header                    Message                  CRC
  注：
  A.当 APP、测量软件发送查询命令至接收机，如对应查询的功能项在接收机端存在多
    个时，需在 Message 中指明要查询的功能模块编号，比如接收机内置两个 4G 模块，
    对 4G 功能的查询需指明模块编号，当对应查询的功能项在接收机端是唯一存在，比
    如接收机的相关版本信息，工作模式，卫星状态等信息，Message 数据全部置 0x00。

                    Name             Message(query)
                    Byte         1                 1
              Description   Module Index       Reserved

  B.接收机对 APP、测量软件查询命令的响应信息 Message 中应包含所查询的参数信息。
  C.APP、测量软件配置接收机参数，接收机进行设置后，返回设置结果
                    Name             Message(Result)
                    Byte         1                 1
              Description      Result          Reserved

                    Name                          Result
                    Value      0x01              0x02        0x03
              Description    设置成功              设置失败          预留

  D.当 APP、测量软件与接收机某一方对数据包的校验不通过或参数解析不匹配时，在
    返回信息的 Message Header 中 Type 字段标明该信息为错误报告，Message 中仅包含
    错误码数据。
                    Name             Message(Error)
                    Byte         1                 3
              Description    Error Code        Reserved

                    Name                        Error Code
                    Value      0x01              0x02        0x03
                    Description   CRC 校验错误         MSG ID 错误       Module Index 错误

四、传输
    本协议数据定义采用小端模式。
五、定义
Messgae Header Definition：
     Name           Type             Description                              Byte   Offset
1    SYNC           char             0xAA                                     1      0
2    SYNC           char             0x44                                     1      1
3    SYNC           char             0x18                                     1      2
4    Header Len     Uchar            协议头部长度                                   1      3
5    MSG ID         Ushort           指令信息 ID                                  2      4
6    Reserved                                                                 2      6
7    Reserved       Uint             时间标签信息预留                                 4      8
8    MSG Len        Ushort           有效信息长度，不包含协议头及 CRC                       2      12
                                     校验值长度
9    Reserved                                                                 2      14
                                                                                              带格式表格[那些]
11   Sender         Uchar            信息发送方                                    1      16
12   Type           Uchar            信息类型                                     1      17
13   Protocol V     Uchar            协议版本                                     1      18
14   MSG Interval   char             报文上传间隔 v                                 1      19
注：
Sender：表明该信息发送方是 APP 或主机，0x00：配置 APP，0x01：主机，0x02：测量软
件 PAD；
Type：表明该信息类型
      0x00：查询，0x01：查询响应；
      0x02：设置，0x03：设置响应；
      0x05：错误报告；
MSG Interval：只对获取 GPGGA/GPRMC/GPGST/HEADING/M925 报文的设置指令有效，
该 值 为 非 0 时 ， 接 收 机 根 据 设 置 的 时 间 间 隔 ， 主 动 上 报 标 准
GPGGA/GPRMC/GPGST/HEADING/M925 报文（非二次封装）。
<0：Interval = fabs(v); >0：Interval = 1/v。
该值可填充 1，默认 1HZ 上报。

1、型号版本信息
MSD ID：0x01
Support: 查询
PAD ->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info):
     Name                    Type    Description   Byte   Offset      query
1    主机型号                    Uchar                 8      0           仅查询
2    主机 SN 号                 Uchar                 16     8           仅查询
3       主机硬件版本号             Uchar                  8    24          仅查询
                                                                                          带格式表格[那些]
4       主机固件版本号             Uchar                  16   32          仅查询
5       WEB 版本号             Uchar                  8    48          仅查询        预留
6       板卡型号                Uchar                  8    56          仅查询
                                                                                          带格式表格[那些]
7       板卡 SN 号             Uchar                  16   64          仅查询
8       板卡固件版本号             Uchar                  16   80          仅查询
9       电台型号                Uchar                  8    96          仅查询
10      电台固件版本号             Uchar                  16   104         仅查询
11      协控制器硬件版本号           Uchar                  4    120         仅查询        预留
12      协控制器固件版本号           Uchar                  4    124         仅查询        预留
13      Reserved            Uchar                  8    128         仅查询
注：
本信息中各类项采用            值标识，若有空余字节填充 0x00。

2、功能信息
MSD ID：0x02
Support:查询
PAD ->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info):
        Name         Type           Description               Byt   Offset   query
                                                              e
1       S/R/S&R      Uchar          基站/移动站/基站&移动站             1     0        仅查询
2       P/D          Uchar          定位/定向                     1     1        仅查询
    3   GNSS         Uchar          GNSS 板卡                   1     2        仅查询
4       IMU          Uchar          惯导                        1     3        仅查询
5       Radio        Uchar          电台                        1     4        仅查询
6       4G/5G        Uchar          4G/5G Module              1     5        仅查询
7       PPS          Uchar          秒脉冲输出                     1     6        仅查询
8       Event        Uchar          事件触发输入                    1     7        仅查询     预留
9       Ext Clock    Uchar          外部时钟                      1     8        仅查询     预留
10      Battery      Uchar          电池                        1     9        仅查询     预留
11      WiFi         Uchar          WiFi                      1     10       仅查询     预留
12      Bluetooth    Uchar          蓝牙                        1     11       仅查询
13      Display      Uchar          显示                        1     12       仅查询
14      PPP          Uchar          PPP 功能                    1     13       仅查询
15      Reserved     Uchar                                    2     14       仅查询     预留
15      RS232        Uchar                                    1     16       仅查询
16      RS485        Uchar                                    1     17       仅查询     预留
17      RS422        Uchar                                    1     18       仅查询     预留
18      Ethernet     Uchar          以太网                       1     19       仅查询     预留
19      CAN          Uchar          CAN 总线                    1     20       仅查询     预留
20   USB Host         Uchar                     1   21   仅查询   预留
21   USB Device       Uchar                     1   22   仅查询
22   Reserved         Uchar                     1   23   仅查询
23   TCP              Uchar                     1   24   仅查询
24   UDP              Uchar                     1   25   仅查询   预留
25   NtripClient      Uchar                     1   26   仅查询
26   NtripServer      Uchar                     1   27   仅查询   预留
27   NtripCaster      Uchar                     1   28   仅查询   预留
28   MQTT             Uchar                     1   29   仅查询   预留
29   JT808            Uchar                     1   30   仅查询
30   Reserved         Uchar                     1   31   仅查询
31   Record           Uchar                     1   32   仅查询   预留
32   Reserved         Uchar                     1   33   仅查询
33   Reserved         Uchar                     1   34   仅查询
34   Reserved         Uchar                     1   35   仅查询
35   Reserved         Uchar                     1   36   仅查询
36   FTP Push         Uchar     FTP 推送          1        仅查询   预留
37   E-mail Alarm     Uchar     邮件告警            1        仅查询   预留
38   cloud            Uchar     云               1        仅查询   预留
39   Remote Upgrade   Uchar     远程升级            1        仅查询
40   Reserved         Uchar                     2        仅查询
注：
A、S/R/S&R：0x01-基准站，0x02-移动站，0x03-基准站&移动站
B、P/D：0x01-定位，0x02-定位定向
C、其余各功能项目的 1 字节的表示含义，最高 bit 位表示当前功能的工作状态，该 bit 位为
1 表示使能或初始化成功，该 bit 位为 0 则表示初始化失败或当前处于不工作状态，剩余 7bit
位表示该功能项在本接收机的个数，0x00 表示不支持该功能，非零值表示该功能项个数，
比如部分特殊接收机可能集成 2 个 4G 模块，例如：
二进制 1000 0001 表示该功能项只有 1 个，且当前处于正常工作状态；
二进制 0000 0001 表示该功能项只有 1 个，且当前处于非工作状态；
二进制 0000 0000 表示接收机不支持该功能项；
二进制 1000 0010 表示该功能项本接收机支持两个，且至少有一个处于正常工作状态；
IMU 字段为 0x82 时，表示接收机内置 IMU 模块为原极模块，在惯导初始化和对中杆标定
流程上与菲曼模块有所区别。
IMU 字段为 0x83 时，表示接收机内置 IMU 模块为和芯 UM981 模块，在惯导初始化和对中
杆标定流程上与菲曼模块有所区别。
IMU 字段为 0x86 时，表示接收机内置 IMU 模块为和芯 K803D 模块，在惯导初始化和对中
杆标定流程上与菲曼模块有所区别。

3、电源及存储空间信息
MSD ID：0x03
Support:查询
PAD ->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info):
     Name             Type      Description   Byte   Offset   query
1    Power            Uchar     外接电源/电池供电     1      0        仅查询
2    Voltage          Uchar     外接电源电压        1      1        仅查询
3    BatteryA level   Uchar     电池电量          1      2        仅查询     预留
4    BatteryB level   Uchar     电池电量          1      3        仅查询     预留
5    Storage CAP      float     总存储容量         4      4        仅查询     预留
6    Available CAP    float     可用存储容量        4      8        仅查询     预留
7    Temperature      char      电池温度          1      12       仅查询
                                                                           带格式表格[WPS_1651113051]
8    Reserved         Uchar     预留            3      13
注：Powe：0x00-外部供电，0x01-电池供电                                                 删除[WPS_1651113051]: Reserved
  Voltage：外部供电电压，正常范围一般是 9~36，当电池供电时，该值为 0
                                                                           删除[WPS_1651113051]: U
  Battery level：正常数值范围为 0~100，0 表示 0%，100 表示 100%
  Temperature：电池温度，10 进制有符号整数，单位为℃                                         删除[WPS_1651113051]: 4

4、定位状态信息
MSD ID：0x04
Support:查询
PAD ->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info) :
GPGGA
注意：当 PAD ->Receiver 该指令的 MSG Header 中的 MSG Interval 字段为非 0 值时，接收机
主动发送标准 GPGGA 报文（无 MSG Header 和 CRC 二次封装），默认 1HZ。

5、可见卫星信息
MSD ID：0x05
Support:查询
PAD ->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info) :
M925B 或 SATSINFOA
注意：
 该指令获取到的 Message(info)格式与板卡型号相关，如 K803/K823 板卡，Message(info)
    格式为 M925B 格式，如为和芯 UM980/UM982 板卡，Message(info)格式为 SATSINFOA
    格式
 当 PAD ->Receiver 该指令的 MSG Header 中的 MSG Interval 字段为非 0 值时，接收机主
    动发送标准 M925B 或 SATSINFOA 报文（无 MSG Header 和 CRC 二次封装），默认 5s
    一次。
 M925B 与 SATSINFOA 格式定义参考司南 K8 系列指令手册与和芯 SATSINFOA 定制协
    议指令手册
和芯 SATSINFOA 报文格式见《Unicore Reference Commands Manual For N4 High Precision
Products_V2_CH_P1.0.A20.pdf》第 153 页

19、HEADING 信息
MSD ID：0x1F
Support:查询
PAD->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info) :
HEADING
注意：当 PAD ->Receiver 该指令的 MSG Header 中的 MSG Interval 字段为非 0 值时，接收机
主动发送标准 HEADING 报文（无 MSG Header 和 CRC 二次封装），默认 1HZ

20、IMU 配置-无需实现
21、GPGST 信息
MSD ID：0x21
Support:查询
PAD ->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info) :
GPGST
注意：当 PAD ->Receiver 该指令的 MSG Header 中的 MSG Interval 字段为非 0 值时，接收机
主动发送标准 GPGST 报文（无 MSG Header 和 CRC 二次封装），默认 1HZ

22、GPRMC 信息
MSD ID：0x22
Support:查询
PAD ->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info) :
GPRMC
注意：当 PAD ->Receiver 该指令的 MSG Header 中的 MSG Interval 字段为非 0 值时，接收机
主动发送标准 GPRMC 报文（无 MSG Header 和 CRC 二次封装），默认 1HZ

23、GPGSA 信息
MSD ID：0x23
Support:查询
PAD ->Receiver: MSG Header|Message(query)|CRC
Receiver->PAD: MSG Header|Message(info)|CRC
Message(info) :
GPGSA
注意：当 PAD ->Receiver 该指令的 MSG Header 中的 MSG Interval 字段为非 0 值时，接收机
主动发送标准 GPGSA 报文（无 MSG Header 和 CRC 二次封装），默认 3s 更新。

24、倾斜测量设置天线高
```
