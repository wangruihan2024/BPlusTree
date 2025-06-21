FROM ubuntu:20.04

# 关闭 tzdata 交互（防止卡住）
ENV DEBIAN_FRONTEND=noninteractive

# 安装必要工具：cmake、编译器、gdb、git 等
RUN apt update && apt install -y \
    build-essential \
    cmake \
    gdb \
    git \
    curl \
    vim \
 && apt clean && rm -rf /var/lib/apt/lists/*
