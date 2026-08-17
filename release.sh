#!/bin/bash

echo "开始编译工程..."
idf.py build

if [ $? -ne 0 ]; then
    echo "========================================="
    echo "❌ 编译失败，请检查报错信息！"
    echo "========================================="
    exit 1
fi

echo "编译成功！正在生成 merge.bin..."

# 切换到 build 目录下执行，解决 flash_args 相对路径找不到文件的问题
cd build
esptool.py --chip esp32s3 merge-bin -o ../merge.bin @flash_args
MERGE_RESULT=$?
cd ..

if [ $MERGE_RESULT -eq 0 ]; then
    echo "========================================="
    echo "✅ 合并成功！"
    echo "固件文件已生成: $(pwd)/merge.bin"
    echo "请回到 Windows，使用官方 Flash Download Tools 烧录。"
    echo "【烧录地址请务必填写: 0】或【0x0】"
    echo "========================================="
else
    echo "========================================="
    echo "❌ 合并失败，请查看上方报错信息。"
    echo "========================================="
fi
