import hilog from '@ohos.hilog';
import serial from 'libserial.so';

@Entry
@Component
struct Index {
  @State portStatus: string = '正在打开串口...';
  @State recvData: string = '';
  @State isOpen: boolean = false;

  private readonly PORT: string = '/dev/ttyS3';
  private readonly BAUD: number = 115200;

  aboutToAppear() {
    hilog.info(0x0000, 'SerialTest', '========== 页面启动 ==========');
    this.openAndListen();
  }

  aboutToDisappear() {
    hilog.info(0x0000, 'SerialTest', '========== 页面退出 ==========');

    try {
      serial.offData(this.PORT);
    } catch (e) {
      hilog.error(0x0000, 'SerialTest', 'offData error: %{public}s', e.message);
    }

    try {
      serial.close(this.PORT);
    } catch (e) {
      hilog.error(0x0000, 'SerialTest', 'close error: %{public}s', e.message);
    }

    this.isOpen = false;
  }

  // 打开串口并监听数据
  openAndListen() {
    try {
      hilog.info(
        0x0000,
        'SerialTest',
        '准备打开串口: %{public}s, baud=%{public}d',
        this.PORT,
        this.BAUD
      );

      // 打开串口
      const ret: boolean = serial.open(this.PORT, this.BAUD);

      hilog.info(
        0x0000,
        'SerialTest',
        'serial.open() 返回: %{public}s',
        ret ? 'true' : 'false'
      );

      if (!ret) {
        this.isOpen = false;
        this.portStatus = '打开失败: ' + this.PORT;
        return;
      }

      this.isOpen = true;
      this.portStatus =
        '串口已打开: ' + this.PORT + ' @ ' + this.BAUD;

      // 注册串口接收回调
      hilog.info(
        0x0000,
        'SerialTest',
        '准备调用 serial.onData()'
      );

      serial.onData(
        this.PORT,
        (port: string, data: string) => {
          hilog.info(
            0x0000,
            'SerialTest',
            '========== 收到串口数据 =========='
          );

          hilog.info(
            0x0000,
            'SerialTest',
            'port=%{public}s',
            port
          );

          hilog.info(
            0x0000,
            'SerialTest',
            'data=%{public}s',
            data
          );

          hilog.info(
            0x0000,
            'SerialTest',
            'data.length=%{public}d',
            data.length
          );

          // 追加接收数据
          this.recvData += data;

          // 最大保留 4096 个字符
          if (this.recvData.length > 4096) {
            this.recvData = this.recvData.substring(
              this.recvData.length - 4096
            );
          }
        }
      );

      hilog.info(
        0x0000,
        'SerialTest',
        'serial.onData() 调用完成，等待串口数据...'
      );

      this.portStatus = '串口监听中: ' + this.PORT;

    } catch (e) {
      this.isOpen = false;

      hilog.error(
        0x0000,
        'SerialTest',
        '========== 串口异常 =========='
      );

      hilog.error(
        0x0000,
        'SerialTest',
        'name=%{public}s',
        e.name
      );

      hilog.error(
        0x0000,
        'SerialTest',
        'message=%{public}s',
        e.message
      );

      hilog.error(
        0x0000,
        'SerialTest',
        'code=%{public}d',
        e.code
      );

      this.portStatus = '打开失败: ' + e.message;
    }
  }

  // 发送数据
  sendData() {
    try {
      if (!this.isOpen) {
        this.portStatus = '串口没有打开';
        return;
      }

      const data: string = 'Hello S3\r\n';

      const n: number = serial.send(
        this.PORT,
        data
      );

      this.portStatus =
        '发送成功，发送字节数: ' + n;

      hilog.info(
        0x0000,
        'SerialTest',
        '发送数据: %{public}s',
        data
      );

      hilog.info(
        0x0000,
        'SerialTest',
        '发送字节数: %{public}d',
        n
      );

    } catch (e) {
      hilog.error(
        0x0000,
        'SerialTest',
        '========== 发送异常 =========='
      );

      hilog.error(
        0x0000,
        'SerialTest',
        'name=%{public}s',
        e.name
      );

      hilog.error(
        0x0000,
        'SerialTest',
        'message=%{public}s',
        e.message
      );

      hilog.error(
        0x0000,
        'SerialTest',
        'code=%{public}d',
        e.code
      );

      this.portStatus =
        '发送失败: ' + e.message;
    }
  }

  // 清空接收区
  clearRecv() {
    this.recvData = '';

    hilog.info(
      0x0000,
      'SerialTest',
      '接收区已清空'
    );
  }

  // 检查串口状态
  checkSerial() {
    try {
      const opened: boolean = serial.isOpen(this.PORT);

      this.isOpen = opened;

      if (opened) {
        this.portStatus = '串口状态：已打开';
      } else {
        this.portStatus = '串口状态：未打开';
      }

      hilog.info(
        0x0000,
        'SerialTest',
        'isOpen=%{public}s',
        opened ? 'true' : 'false'
      );

    } catch (e) {
      hilog.error(
        0x0000,
        'SerialTest',
        'isOpen error: %{public}s',
        e.message
      );
    }
  }

  build() {
    Column() {

      Text('OpenHarmony 串口助手')
        .fontSize(28)
        .fontWeight(FontWeight.Bold)
        .margin({
          bottom: 20
        })

      Text(this.portStatus)
        .fontSize(18)
        .width('100%')
        .margin({
          bottom: 15
        })

      Text(
        '串口: ' + this.PORT +
          '    波特率: ' + this.BAUD
      )
        .fontSize(16)
        .margin({
          bottom: 15
        })

      Button('发送 Hello S3')
        .fontSize(22)
        .width('100%')
        .height(55)
        .onClick(() => {
          this.sendData();
        })

      Button('检查串口状态')
        .fontSize(20)
        .width('100%')
        .height(50)
        .margin({
          top: 10
        })
        .onClick(() => {
          this.checkSerial();
        })

      Button('清空接收区')
        .fontSize(20)
        .width('100%')
        .height(50)
        .margin({
          top: 10
        })
        .onClick(() => {
          this.clearRecv();
        })

      Text('接收数据:')
        .fontSize(22)
        .fontWeight(FontWeight.Bold)
        .width('100%')
        .margin({
          top: 20,
          bottom: 10
        })

      Scroll() {
        Text(
          this.recvData.length > 0
            ? this.recvData
            : '(暂无数据)'
        )
          .fontSize(18)
          .width('100%')
          .textAlign(TextAlign.Start)
      }
      .width('100%')
      .layoutWeight(1)
      .border({
        width: 1,
        color: Color.Gray
      })
      .padding(10)

    }
    .width('100%')
    .height('100%')
    .padding(20)
    .justifyContent(FlexAlign.Start)
  }
}