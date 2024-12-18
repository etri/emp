# Elastic Memory Platform (EMP)

이 프로젝트는 다계층 이종 메모리 관리 기술을 기반으로 패브릭 메모리 시스템을 구축하는 Elastic Memory Platform(EMP)입니다. \
EMP는 가상머신(Virtual Machine), 애플리케이션, 컨테이너(Container)가 대규모 메모리를 효율적으로 사용할 수 있도록 하는 기술입니다. \
EMP의 실행 모드는 가상머신 기반 (VM EMP), 애플리케이션/컨테이너 기반 (user-level EMP)의 두 가지 모드로 지원됩니다. 

EMP의 Basic Profile 은 다음과 같은 특징을 제공합니다.
- Static DRAM Mgmt.
- Dynamic DRAM Mgmt.
- Various Block (from 4KB to 2MB)
- Various Subblock (from 4KB to 128KB)
- vCPU Scalability
- 2nd tier Memory: Remote DRAM / HPSSD 
<br>

## Table of Contents
- [Environment & Prerequisites](#environment-and-prerequisites)
- [Installation](#installation)
- [Usage](#usage)
- [Authors](#authors)
<br>

## Environment and Prerequisites
- 개발언어:  C
- 지원하드웨어
-- Intel 혹은 AMD CPU (x86\_64)
-- Mellanox InfiniBand (ConnectX-3, 4, 5, 6 or 7), RoCE NIC, Intel iWARP NIC
-- HPSSD (NVMe SSD)
- OS:  
-- RHEL/CentOS/Rocky 8 (4.18.0-240.10.1.el8.x86\_64, 4.18.0-477.27.1.el8.x86\_64)
-- RHEL/Rocky 9 (5.14.0-284.11.1.el9\_2.x86\_64, 5.14.0-284.30.1.el9\_2.x86\_64)
- 커널 컴파일을 위한 관련 패키지 설치
```
$ sudo dnf install kernel-devel -y
```
- QEMU 컴파일을 위한 관련 패키지 설치
```
$ sudo dnf --enablerepo=powertools builddep qemu-kvm -y
$ sudo dnf --enablerepo=powertools install spice-server-devel spice-protocol usbredir-devel -y
```
- InfiniBand 사용을 위한 패키지 설치
```
$ sudo dnf install iproute libibverbs libibverbs-utils infiniband-diags
```
<br>

## Installation 
- 다음과 같이 커널 모듈을 컴파일합니다.
```
$ make
```

- (User-level EMP 실행 모드인 경우) 다음과 같이 mimalloc library 를 컴파일 합니다.
```
$ cd mimalloc_emp
$ mkdir out; cd out 
$ cmake ..; make; sudo make install
```

- 다음과 같이 Stock KVM 커널 모듈을 언로딩합니다. 
```
$ sudo modprobe -r kvm\_{ARCH}
$ sudo modprobe -r kvm
```

- 다음과 같이 EMP를 위한 KVM 커널 모듈을 로딩합니다. 
```
$ sudo insmod module\_kvm/${uname -r}/kvm.ko
$ sudo insmod module\_kvm/${uname -r}/kvm\_{ARCH}.ko
```

- 다음과 같이 emp 커널 모듈을 로딩합니다.
```
$ sudo insmod module\_gpl/emp.ko
$ sudo insmod module\_pro/emp\_pro.ko
```
<br>

## Usage 
- (메모리 제공 노드) memory exporter 를 실행
```
$ ${VMD_BIN} -m <메모리 크기 (MB)> -p <포트 번호> -d <IB 장치>
Example:
$ ${EMP\_HOME}/vmd/vmd -m $((16*1024)) -p 19675 -d mlx5_0
```

- (VM EMP 실행 모드인 경우: 가상머신 노드) mem-path argument를 이용하여 메모리 확장 가상머신을 실행 
```
$ sudo ${EMP\_QEMU} -enable-kvm ... --mem-path DONOR\_IPADDR:DONOR\_SIZE:DONOR\_PORT
Example (Remote DRAM: 128GB, HPSSD 128GB, DRAM 32GB):
$ ./qemu-system-x86\_64 -enable-kvm -nodefaults -nographic \
 -L /usr/share/qemu -qmp tcp:127.0.0.1:5566,server,nowait \
 -m 256G -smp 24 -boot c -drive file=ubuntu.qcow2,if=virtio,cache=none \
 -drive file=/dev/nvme0n1,if=virtio,cache=none,format=raw \
 -netdev user,id=hostnet0,hostfwd=tcp::5556-:22 \
 -device virtio-net-pci,netdev=hostnet0,id=net0,bus=pci.0,addr=0x3 \
 --mem-path 10.10.0.112:131072:19675|/dev/nvme1n1:131072:0|dram:32768
```

- (User-level EMP 실행 모드인 경우: 응용프로그램 실행 노드) mem-path argument 및 LD_PRELOAD를 이용하여 메모리 확장 기능을 실행 
```
Example (Remote DRAM: 512GB, DRAM 16GB, mimalloc lib 사용)
$ EMP_PMEM_PATH="dram:16384|10.10.0.112:524288:19675" LD_PRELOAD=/usr/loca/lib64/libmimalloc.so ./APP
```
<br>

## Authors
* 고광원 &nbsp;&nbsp;&nbsp; kwangwon.koh@etri.re.kr
* 김강호 &nbsp;&nbsp;&nbsp; khk@etri.re.kr
* 김창대 &nbsp;&nbsp;&nbsp; cdkim@etri.re.kr
* 김태훈 &nbsp;&nbsp;&nbsp; taehoon.kim@etri.re.kr
* 정연정 &nbsp;&nbsp;&nbsp; yjjeong@etri.re.kr
* 박은지 &nbsp;&nbsp;&nbsp; pakeunji@etri.re.kr
<br>
