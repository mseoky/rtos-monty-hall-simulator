# RTOS Monty Hall Simulator

> **"임베디드 RTOS 환경에서 몬티홀 딜레마를 실시간으로 체험하는 인터랙티브 시뮬레이터"**

[![C](https://img.shields.io/badge/C-11-A8B9CC?logo=c&logoColor=black)](https://en.cppreference.com/w/c)
[![STM32](https://img.shields.io/badge/MCU-STM32F429ZI-03234B?logo=stmicroelectronics&logoColor=white)](https://www.st.com/)
[![RTOS](https://img.shields.io/badge/RTOS-uC%2FOS--III-1E90FF)](https://www.micrium.com/)
[![USART](https://img.shields.io/badge/Debug-UART%20115200-4CAF50)]()

## 팀원

- 부산대학교 21학번 윤민석
- 부산대학교 21학번 김경환

---

## 프로젝트 소개

**RTOS Monty Hall Simulator**는 STM32F429ZI Nucleo 보드와 uC/OS-III 기반으로 구현한 실시간 몬티홀 딜레마 시뮬레이터입니다.

조이스틱/버튼 입력, ANSI UART 터미널 UI, 물리 LED 피드백을 결합해 다음을 동시에 검증합니다.

- 몬티홀 딜레마의 확률적 특성 (`Stay` vs `Switch`)
- RTOS 태스크 분할 및 우선순위 스케줄링
- 세마포어 기반 태스크 동기화
- 크리티컬 섹션 기반 공유자원 보호

---

## 핵심 기술 포인트

| 항목 | 적용 내용 |
|------|------|
| **RTOS 스케줄링** | uC/OS-III 선점형 우선순위 스케줄링 |
| **태스크 구조** | `AppTask_INPUT`, `AppTask_GameLogic`, `AppTask_GAME`, `AppTask_LED` |
| **IPC/동기화** | 바이너리 세마포어 8개 (`OSSemCreate`, `OSSemPend`, `OSSemPost`) |
| **공유 데이터 보호** | `OS_CRITICAL_ENTER/EXIT`로 `gamePhase`, 커서, 결과, 통계 보호 |
| **난수 생성** | STM32 하드웨어 RNG (`RNG_GetRandomNumber`)로 `prizeDoor` 결정 |
| **입력 처리** | 조이스틱 ADC(PC0), 버튼 GPIO(PF13) 엣지 감지, 10ms 주기 폴링 |
| **출력 처리** | USART3(115200) ANSI UI + LED(PB0/PB14) 2초 결과 표시 |
| **통계 검증** | 라운드/승/패/승률 실시간 누적 출력 |

---

## 태스크 및 우선순위

| Task | 우선순위 | 역할 |
|------|------|------|
| `AppTask_INPUT` | `0` | 조이스틱/버튼 입력 처리, 단계별 이벤트 세마포어 포스트 |
| `AppTask_GameLogic` | `3` | 라운드 초기화, 호스트 공개 로직, 최종 승패 계산, 통계 누적 |
| `AppTask_GAME` | `4` | ANSI 터미널 렌더링(선택/교체/결과 단계 UI) |
| `AppTask_LED` | `5` | 결과 단계 LED 피드백(승: Green, 패: Red) |

> 입력 태스크를 최상위로 두어 사용자 반응성을 확보하고, 렌더링/LED는 그 다음 우선순위로 분리했습니다.

---

## 세마포어 기반 상호작용

| 세마포어 | 의미 |
|------|------|
| `Sem_UserSelectDone` | 사용자의 1차 문 선택 완료 |
| `Sem_DisplaySelectDone` | 선택 단계 화면 갱신 요청 |
| `Sem_SwitchSelectDone` | Stay/Switch 선택 완료 |
| `Sem_DisplaySwitchDone` | 교체 단계 화면 갱신 요청 |
| `Sem_ResultReady` | 결과 데이터 준비 완료 |
| `Sem_LedShow` | LED 결과 표시 트리거 |
| `Sem_NextRoundDisp` | 다음 라운드 화면 진행 신호 |
| `Sem_NextRoundLogic` | 다음 라운드 로직 진행 신호 |

---

## 게임 상태 머신

### `PHASE_SELECT`
- 조이스틱으로 문 커서 이동 (`1~3` 순환)
- 버튼 입력 시 `userChoice` 확정

### `PHASE_REVEAL`
- 진행자가 염소 문(`hostChoice`) 공개
- 사용자는 `Stay`/`Switch` 선택

### `PHASE_RESULT`
- 최종 문 계산 및 승패 판정
- UART 결과 렌더링 + LED 2초 표시
- 버튼으로 다음 라운드 진행

---

## 하드웨어/주변장치 매핑

| 디바이스 | 핀/인터페이스 | 용도 |
|------|------|------|
| Joystick VRx | `PC0 (ADC1_IN10)` | 좌/우 이동 입력 |
| Push Button | `PF13 (Pull-up)` | 선택/확인 입력 |
| UART Terminal | `USART3 (PD8/PD9, 115200)` | ANSI 텍스트 UI/로그 |
| Green LED | `PB0` | 승리 표시 |
| Red LED | `PB14` | 패배 표시 |

---

## 🏗 프로젝트 구조 (Structure)

이 프로젝트는 STM32 보드별 예제 코드(`Examples`)와 RTOS/플랫폼 소스(`Software`)를 분리한 구조로 구성되어 있습니다.

사용 RTOS는 **uC/OS-III**이며, 애플리케이션 엔트리 파일은 `Examples/ST/STM32F429II-SK/OS3/app.c`입니다.

```text
rtos-monty-hall-simulator/
├── Examples/
│   └── ST/
│       └── STM32F429II-SK/
│           ├── BSP/                       # 보드 지원 패키지(GPIO, Tick, Interrupt)
│           ├── OS3/
│           │   ├── app.c                  # Monty Hall 애플리케이션 메인 로직
│           │   ├── includes.h             # 공통 include (BSP + uC/OS-III)
│           │   ├── os_cfg.h               # RTOS 설정
│           │   ├── cpu_cfg.h / lib_cfg.h  # CPU/LIB 설정
│           │   ├── KeilMDK/               # Keil 프로젝트 파일(uvproj)
│           │   ├── IAR/                   # IAR 프로젝트 파일(ewp)
│           │   └── TrueSTUDIO/            # TrueSTUDIO 프로젝트 파일
│           ├── stm32f4xx_it.c
│           ├── system_stm32f4xx.c
│           └── tiny_printf.c
├── Software/
│   ├── uC-CPU/                            # Cortex-M4 CPU 포트 레이어
│   ├── uC-LIB/                            # Micrium 유틸리티 라이브러리
│   └── uCOS-III/
│       ├── Source/                        # RTOS 커널 소스 (task/sem/time/...)
│       └── Ports/ARM-Cortex-M4/Generic/   # Cortex-M4 포팅 소스
├── report.pdf
└── README.md
```

---

## 🚀 실행 방법 (Getting Started)

### 1. 준비물 / 개발 환경

- 보드: `STM32F429ZI Nucleo`
- 디버거: 보드 내장 ST-LINK (USB)
- 터미널: Tera Term / PuTTY (UART 115200, 8N1)
- IDE(택1)
  - `Keil uVision` (권장, `KeilMDK/OS3.uvproj` 사용)
  - `IAR Embedded Workbench` (`IAR/OS3.ewp` 사용)
  - `STM32 TrueSTUDIO` 또는 호환 IDE (`TrueSTUDIO` 프로젝트 사용)

### 2. 저장소 클론

```bash
git clone https://github.com/mseoky/rtos-monty-hall-simulator.git
cd rtos-monty-hall-simulator
```

### 3. 프로젝트 열기

- Keil: `Examples/ST/STM32F429II-SK/OS3/KeilMDK/OS3.uvproj`
- IAR: `Examples/ST/STM32F429II-SK/OS3/IAR/OS3.ewp`
- TrueSTUDIO: `Examples/ST/STM32F429II-SK/OS3/TrueSTUDIO/.project`

### 4. 코드 수정 위치

몬티홀 게임 로직 및 태스크 구현의 중심 파일:

- `Examples/ST/STM32F429II-SK/OS3/app.c`

### 5. 빌드/다운로드/실행

1. IDE에서 `STM32F429ZI` 타깃이 선택되어 있는지 확인
2. 전체 빌드(Build/Rebuild)
3. ST-LINK로 보드에 다운로드(Flash)
4. 디버그 또는 Run으로 실행

### 6. UART 모니터링 설정

터미널 프로그램에서 아래로 접속:

- Baudrate: `115200`
- Data bits: `8`
- Parity: `None`
- Stop bits: `1`
- Flow control: `None`

실행 후 ANSI 기반 Monty Hall UI와 라운드 통계가 출력됩니다.

### 7. 하드웨어 입력 연결 확인

- 조이스틱 VRx -> `PC0 (ADC1_IN10)`
- 버튼 -> `PF13` (내부 Pull-up)
- 결과 LED -> `PB0(Green)`, `PB14(Red)`

---

## 실행 결과

- `Stay` 전략: 약 **33%** 이론값 (실험 예시 10회: **40%**)
- `Switch` 전략: 약 **67%** 이론값 (실험 예시 10회: **70%**)
- 반복 라운드에서 통계가 이론값으로 수렴하는 경향 확인

시연 영상:
- 기본 동작: <br>
[![Monty Hall Demo](https://img.youtube.com/vi/mQQAJEg9-_M/0.jpg)](https://youtu.be/mQQAJEg9-_M)
- Stay 전략(10 round): <br>
[![Stay Strategy Demo](https://img.youtube.com/vi/RBqlFGTm3jw/0.jpg)](https://youtu.be/RBqlFGTm3jw)
- Switch 전략(10 round): <br>
[![Switch Strategy Demo](https://img.youtube.com/vi/OvIBOasT3uM/0.jpg)](https://youtu.be/OvIBOasT3uM)

---

## 코드 구조 (핵심 함수)

- `main()` : 보드/OS 초기화, 시작 태스크 생성
- `AppTaskCreate()` : 태스크 생성 및 우선순위 할당
- `AppObjCreate()` : 세마포어 생성
- `AppTask_INPUT()` : 입력 이벤트 처리 및 단계별 세마포어 포스트
- `AppTask_GameLogic()` : 라운드 진행/승패 판정/통계 누적
- `AppTask_GAME()` : ANSI 터미널 UI 렌더링
- `AppTask_LED()` : 결과 LED 피드백
- `RNG_HwInit()`, `RNG_GetRandom32()` : 하드웨어 RNG 초기화/사용
- `RenderScreen()`, `MakeStatsLine()` : 화면 및 통계 문자열 구성

---

## 참고

- [`report.pdf`](report.pdf) (설계 배경, 검증 과정, 실험 결과)

