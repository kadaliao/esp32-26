#pragma once

// 把最后渲染的一帧（RLE 压缩 + hex）通过串口发给主机，
// 方便在 Mac 上还原成 PNG 检查显示效果，不需要拍照。
void shot_send(void);
