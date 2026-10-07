import ble from '@ohos.bluetooth.ble';
import hilog from '@ohos.hilog';

const TAG = 'BluetoothTest';

interface DeviceInfo {
  deviceId: string;
  deviceName: string;
  rssi: number;
}

@Entry
@Component
struct Index {
  @State devices: DeviceInfo[] = [];
  @State status: string = '未开始扫描';

  // 保存回调引用
  private bleDeviceFindCallback: (results: Array<ble.ScanResult>) => void =
    (results: Array<ble.ScanResult>) => {
      try {
        hilog.info(
          0x0000,
          TAG,
          '收到 BLE 扫描结果，数量: %{public}d',
          results.length
        );

        for (let result of results) {
          const device: DeviceInfo = {
            deviceId: result.deviceId,
            deviceName: result.deviceName || '未知设备',
            rssi: result.rssi
          };

          hilog.info(
            0x0000,
            TAG,
            '发现设备: name=%{public}s, id=%{public}s, rssi=%{public}d',
            device.deviceName,
            device.deviceId,
            device.rssi
          );

          // 避免重复添加相同设备
          const exists = this.devices.some(
            item => item.deviceId === device.deviceId
          );

          if (!exists) {
            this.devices.push(device);
          }
        }
      } catch (err) {
        hilog.error(
          0x0000,
          TAG,
          '处理 BLE 扫描结果失败: %{public}s',
          JSON.stringify(err)
        );
      }
    };

  aboutToAppear() {
    try {
      // API 10/11 新接口
      ble.on(
        'BLEDeviceFind',
        this.bleDeviceFindCallback
      );

      hilog.info(
        0x0000,
        TAG,
        'BLEDeviceFind listener registered'
      );

      this.status = '蓝牙扫描监听已注册';
    } catch (err) {
      this.status = '监听注册失败';

      hilog.error(
        0x0000,
        TAG,
        '注册 BLEDeviceFind 监听失败: %{public}s',
        JSON.stringify(err)
      );
    }
  }

  aboutToDisappear() {
    this.stopScan();

    try {
      ble.off(
        'BLEDeviceFind',
        this.bleDeviceFindCallback
      );

      hilog.info(
        0x0000,
        TAG,
        'BLEDeviceFind listener unregistered'
      );
    } catch (err) {
      hilog.error(
        0x0000,
        TAG,
        '注销 BLEDeviceFind 监听失败: %{public}s',
        JSON.stringify(err)
      );
    }
  }

  startScan() {
    try {
      this.devices = [];
      this.status = '正在扫描...';

      /*
       * API 10/11:
       * startBLEScan(filters, options?)
       *
       * 不使用过滤器时传 null。
       */
      ble.startBLEScan(null);

      hilog.info(
        0x0000,
        TAG,
        '开始 BLE 扫描'
      );
    } catch (err) {
      this.status = '扫描失败';

      hilog.error(
        0x0000,
        TAG,
        'BLE 扫描失败: %{public}s',
        JSON.stringify(err)
      );
    }
  }

  stopScan() {
    try {
      ble.stopBLEScan();

      this.status = '扫描已停止';

      hilog.info(
        0x0000,
        TAG,
        '停止 BLE 扫描'
      );
    } catch (err) {
      hilog.error(
        0x0000,
        TAG,
        '停止 BLE 扫描失败: %{public}s',
        JSON.stringify(err)
      );
    }
  }

  build() {
    Column({ space: 10 }) {
      Text('OpenHarmony API 11 蓝牙测试')
        .fontSize(24)
        .fontWeight(FontWeight.Bold)

      Text(this.status)
        .fontSize(18)

      Row({ space: 10 }) {
        Button('开始扫描')
          .onClick(() => {
            this.startScan();
          })

        Button('停止扫描')
          .onClick(() => {
            this.stopScan();
          })
      }

      Text('发现设备：' + this.devices.length)
        .fontSize(18)

      List({ space: 8 }) {
        ForEach(
          this.devices,
          (item: DeviceInfo) => {
            ListItem() {
              Column({ space: 5 }) {
                Text(item.deviceName)
                  .fontSize(18)
                  .fontWeight(FontWeight.Bold)

                Text('地址：' + item.deviceId)
                  .fontSize(14)

                Text('RSSI：' + item.rssi)
                  .fontSize(14)
              }
              .alignItems(HorizontalAlign.Start)
              .width('100%')
              .padding(10)
            }
          },
          (item: DeviceInfo) => item.deviceId
        )
      }
      .layoutWeight(1)
    }
    .padding(20)
    .width('100%')
    .height('100%')
  }
}