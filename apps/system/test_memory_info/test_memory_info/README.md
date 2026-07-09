# test_memory_info

heapinfo 와 동일한 heap 조회 기능에 더해, `-t` 옵션으로 **각 Task 별 할당 목록과 할당 시점의 backtrace 를 한 줄로** 출력하는 테스트 도구입니다.

backtrace 는 heuristic stack scan 이 아니라, malloc 시점에 `sched_backtrace()`
(`CONFIG_SCHED_BACKTRACE`, EHABI unwind) 로 각 allocation node 에 이미 저장된
정확한 call-stack 을 그대로 읽어서 표시합니다.

## 빌드 설정

```
CONFIG_DEBUG_MM_HEAPINFO=y      # heap 조회 (필수)
CONFIG_MM_BACKTRACE=8           # allocation backtrace 저장 (-t 에 필수, >0)
CONFIG_SCHED_BACKTRACE=y        # 정확한 unwind 엔진
CONFIG_SYSTEM_TEST_MEMORY_INFO=y
```

menuconfig 경로: `Application Configuration -> System Libraries and Add-Ons -> Test Memory Info tool`

## 사용법

```
Usage: test_memory_info [OPTION]

  -t             각 Task 별 allocation 목록 + backtrace (한 줄에 표시)
  -a             모든 allocation 상세 (heapinfo -a 와 동일)
  -f             free list 표시
  -p PID         지정한 PID 로 제한 (-t / -a 와 함께)
  -b BIN_NAME    해당 binary 의 heap 조회
  -k             kernel heap 조회 (기본값)
  -i             peak allocated size 초기화
  -h             도움말
```

## 어느 heap 을 봐야 하나 (중요)

`CONFIG_BUILD_PROTECTED` 빌드에서는 backtrace 가 **USER(app) heap 에만 기록**됩니다.
user 공간 malloc 은 `sched_backtrace()` 로 call-stack 을 노드에 저장하지만,
**kernel heap 은 의도적으로 seqno 만 기록**합니다 (early-boot / IRQ / IDLE 컨텍스트에서
EHABI unwind 가 위험하기 때문). 따라서 kernel heap 을 보면 `bt: (none)` 으로 나옵니다.

그래서 `-t` 는 heap 을 명시(`-k`/`-b`)하지 않으면 **자동으로 user app heap
(`CONFIG_APP1_BIN_NAME` 등)을 조회**합니다. `CONFIG_SUPPORT_COMMON_BINARY` +
`CONFIG_NUM_APPS=1` 환경에서는 모든 user task 할당이 하나의 공통 app heap 에 모이므로
`-t` 한 번으로 전체 user task 의 할당 + backtrace 를 볼 수 있습니다.

| 명령 | 조회 대상 | backtrace |
|------|-----------|-----------|
| `test_memory_info -t` | user app heap (자동) | O |
| `test_memory_info -t -b app1` | app1 heap (명시) | O |
| `test_memory_info -t -k` | kernel heap | X (seqno only) |

## 출력 형식 (`-t`)

```
nsh> test_memory_info -t

=== Per-Task Allocation Backtrace ===
(each line is labelled with its owner task)

[app heap: app1]
PID(name) | Alloc Addr | Size | Backtrace (malloc call path)
-------------------------------------------------------------
PID=18(app1) ptr=0x6010a2b0 size=64 seq=137 bt: 0x040012a4 <- 0x04001180 <- 0x04000f30
PID=20(tash) ptr=0x6010a300 size=128 seq=138 bt: 0x040012a4 <- 0x040011c8 <- 0x04000f30
-------------------------------------------------------------
PID -1 live allocations : ... 
```

- `bt: a <- b <- c` : `a` 가 malloc 을 호출한 가장 안쪽 프레임, 오른쪽으로 갈수록 상위 caller
- 특정 task 만 보려면: `test_memory_info -t -p 18`
- 특정 app heap 명시: `test_memory_info -t -b app1`

## backtrace 주소 해석

출력된 주소는 다음으로 심볼/파일:라인 으로 변환합니다.

```
arm-none-eabi-addr2line -f -C -e <ELF> 0x040012a4 0x04001180 ...
```

TizenRT rtl8730e 환경에서는 주소 대역에 따라 알맞은 ELF(common_dbg 등)를
선택하고, loadable app 은 슬롯 offset 을 반영해야 합니다.
