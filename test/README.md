# Test program for Elastic Memory Platform (EMP)

## Installation 
```
$ pushd ../libemp 
$ make 
$ popd
$ make
```

<br>

## Usage 
```
$ ./t1
$ ./t2 
```
- (t3-4) 2-tier memory 를 사용
```
t3는 rvme4n1을 사용, nvme4n1 이 삭제되므로 주의
$ ./t3_memconsumer
Example (DRAM: 24GB, RDRAM: 80GB, Memory Donor: 10.10.0.115, $ ./vmd -m $((80*1024)) -p 19675 -d mlx5_0)
$ EMP_MEM_PATH="dram:$((24<<10))|10.10.0.115:$((80<<10)):19675" LD_PRELOAD=../libemp/libemp.so ./t4_memconsumer
```
<br>

## Authors
* 고광원 &nbsp;&nbsp;&nbsp; kwangwon.koh@etri.re.kr
* 김강호 &nbsp;&nbsp;&nbsp; khk@etri.re.kr
* 김창대 &nbsp;&nbsp;&nbsp; cdkim@etri.re.kr
* 김태훈 &nbsp;&nbsp;&nbsp; taehoon.kim@etri.re.kr
<br>
