#include <includes.h>
#include <stdbool.h>

#include "bsp.h"
#include "stm32f4xx.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_rng.h" /* 하드웨어 RNG */

typedef enum {
    PHASE_SELECT,  // 1단계: 문 선택 대기
    PHASE_REVEAL,  // 2단계: 호스트 문 공개 및 교체 선택 대기
    PHASE_RESULT   // 3단계: 최종 결과 표시
} GamePhase_t;

static volatile GamePhase_t gamePhase;
static volatile uint8_t prizeDoor;
static volatile uint8_t userChoice;
static volatile uint8_t hostChoice;
static volatile bool switchChoice;
static volatile bool gameWin;
static volatile uint8_t finalDoorChoice = 0; /* RESULT 단계에서 ▲ 표시용 */
static volatile uint8_t cursorDoor = 1;      /* 1 ~ 3 */
static volatile uint8_t cursorSwitch = 0;    /* 0=Stay, 1=Switch */

// UCOS-III binary semaphore (입력 대기용)
static OS_SEM Sem_UserSelectDone;
static OS_SEM Sem_DisplaySelectDone;
static OS_SEM Sem_SwitchSelectDone;
static OS_SEM Sem_DisplaySwitchDone;
static OS_SEM Sem_ResultReady;
static OS_SEM Sem_LedShow;
static OS_SEM Sem_NextRoundDisp;  /* 화면(Task_GAME)용 */
static OS_SEM Sem_NextRoundLogic; /* 로직(Task_GameLogic)용 */

// 몬티홀 통계 (모두 AppTask_GAME에서만 업데이트)
volatile uint16_t g_roundCount = 0;
volatile uint16_t g_winCount = 0;
volatile uint16_t g_loseCount = 0;

/* ANSI Escape helpers */
#define ESC "\033"
#define CSI "\033[" /* Control Sequence Introducer */

typedef enum {
    DOOR_CLOSED,     /* ? 표시  */
    DOOR_OPEN_GOAT,  /* GOAT!  */
    DOOR_OPEN_PRIZE, /* $$$$$  */
    DOOR_OPEN_FAIL   /* XX!!   */
} DoorState_t;

typedef enum { JOY_IDLE = 0,
               JOY_LEFT = -1,
               JOY_RIGHT = +1 } JoyDir_t;

/* ─── 화면 모델 ─────────────────────────────────────────── */
static const char *doorArt[4][5] = {
    /* DOOR_CLOSED */
    {
        " ┌─────┐ ",
        " │     │ ",
        " │  ?  │ ",
        " │     │ ",
        " └─────┘ "},
    /* DOOR_OPEN_GOAT */
    {
        " ┌─────┐ ",
        " │     │ ",
        " │\033[34mGOAT!\033[0m│ ",
        " │     │ ",
        " └─────┘ "},
    /* DOOR_OPEN_PRIZE */
    {
        " ┌─────┐ ",
        " │\033[33m$$$$$\033[0m│ ",
        " │\033[33m$$$$$\033[0m│ ",
        " │\033[33m$$$$$\033[0m│ ",
        " └─────┘ "},
    /* DOOR_OPEN_FAIL  ★ */
    {
        " ┌─────┐ ",
        " │\033[31mXXXXX\033[0m│ ",
        " │\033[31mEMPTY\033[0m│ ",
        " │\033[31mXXXXX\033[0m│ ",
        " └─────┘ "}};

static DoorState_t doors[4]; /* 1 ~ 3 사용 */
static char footer[64];      /* 하단 안내 메시지 */

typedef enum {
    COM1 = 0,
    COM2 = 1,
    COM3 = 2,
} COM_TypeDef;

#define COMn 1

#define Nucleo_COM1 USART3
#define Nucleo_COM1_CLK RCC_APB1Periph_USART3
#define Nucleo_COM1_TX_PIN GPIO_Pin_9
#define Nucleo_COM1_TX_GPIO_PORT GPIOD
#define Nucleo_COM1_TX_GPIO_CLK RCC_AHB1Periph_GPIOD
#define Nucleo_COM1_TX_SOURCE GPIO_PinSource9
#define Nucleo_COM1_TX_AF GPIO_AF_USART3
#define Nucleo_COM1_RX_PIN GPIO_Pin_8
#define Nucleo_COM1_RX_GPIO_PORT GPIOD
#define Nucleo_COM1_RX_GPIO_CLK RCC_AHB1Periph_GPIOD
#define Nucleo_COM1_RX_SOURCE GPIO_PinSource8
#define Nucleo_COM1_RX_AF GPIO_AF_USART3
#define Nucleo_COM1_IRQn USART1_IRQn

USART_TypeDef *COM_USART[COMn] = {Nucleo_COM1};
GPIO_TypeDef *COM_TX_PORT[COMn] = {Nucleo_COM1_TX_GPIO_PORT};
GPIO_TypeDef *COM_RX_PORT[COMn] = {Nucleo_COM1_RX_GPIO_PORT};

const uint32_t COM_USART_CLK[COMn] = {Nucleo_COM1_CLK};
const uint32_t COM_TX_PORT_CLK[COMn] = {Nucleo_COM1_TX_GPIO_CLK};
const uint32_t COM_RX_PORT_CLK[COMn] = {Nucleo_COM1_RX_GPIO_CLK};

const uint16_t COM_TX_PIN[COMn] = {Nucleo_COM1_TX_PIN};
const uint16_t COM_RX_PIN[COMn] = {Nucleo_COM1_RX_PIN};

const uint16_t COM_TX_PIN_SOURCE[COMn] = {Nucleo_COM1_TX_SOURCE};
const uint16_t COM_RX_PIN_SOURCE[COMn] = {Nucleo_COM1_RX_SOURCE};

const uint16_t COM_TX_AF[COMn] = {Nucleo_COM1_TX_AF};
const uint16_t COM_RX_AF[COMn] = {Nucleo_COM1_RX_AF};

static void AppTask_GAME(void *p_arg);
static void AppTask_GameLogic(void *p_arg);

static OS_TCB AppTaskStartTCB;
static CPU_STK AppTaskStartStk[APP_CFG_TASK_START_STK_SIZE];

static OS_TCB Task_GAME_TCB;
static CPU_STK Task_GAME_Stack[APP_CFG_TASK_START_STK_SIZE * 10];

static OS_TCB Task_GameLogic_TCB;
static CPU_STK Task_GameLogic_Stack[APP_CFG_TASK_START_STK_SIZE];

static OS_TCB Task_LED_TCB;
static CPU_STK Task_LED_Stk[APP_CFG_TASK_START_STK_SIZE];

static OS_TCB Task_INPUT_TCB;
static CPU_STK Task_INPUT_Stack[APP_CFG_TASK_START_STK_SIZE * 10];

#define LED_GREEN_PIN GPIO_Pin_0 /* PB0  */
#define LED_RED_PIN GPIO_Pin_14  /* PB14 */

static void AppTaskStart(void *p_arg);
static void AppTaskCreate(void);
static void AppObjCreate(void);

static void AppTask_INPUT(void *p_arg);

static void Setup_Gpio(void);
static void Setup_InputHw(void);

static inline void Term_ClearScreen(void) { send_string(CSI "2J" CSI "H"); }
static inline void Term_CursorHome(void) { send_string(CSI "H"); }

static void MakeStatsLine(char *buf, size_t n) {
    CPU_SR_ALLOC();
    OS_CRITICAL_ENTER();
    uint16_t r = g_roundCount;
    uint16_t w = g_winCount;
    uint16_t l = g_loseCount;
    OS_CRITICAL_EXIT();
    int winRate = (r ? (w * 100) / r : 0);
    snprintf(buf, n,
             "[Round:%3u | \033[32mWin:%3u\033[0m | \033[31mLose:%3u\033[0m | \033[34mWin Rate:%3d%%\033[0m]",
             r, w, l, winRate);
}

static void RenderScreen(uint8_t cursorDoor, uint8_t cursorSwitch) {
    char line[128];

    Term_ClearScreen();
    Term_CursorHome();

    /* ─ 전체 지우기 & 타이틀 배너 ─ */
    send_string("\033[46m\033[30m================================================\r\n");
    send_string("              Monty Hall Simulator              \r\n");
    send_string("================================================\033[0m\r\n\r\n");

    CPU_SR_ALLOC();
    GamePhase_t phaseSnap;
    DoorState_t doorSnap[4];
    uint8_t userSnap, hostSnap, finalSnap;
    bool winSnap;
    char footerSnap[64];

    OS_CRITICAL_ENTER();
    phaseSnap = gamePhase;
    memcpy(doorSnap, doors, sizeof doors);
    userSnap = userChoice;
    hostSnap = hostChoice;
    finalSnap = finalDoorChoice;
    winSnap = gameWin;
    strcpy(footerSnap, footer);
    OS_CRITICAL_EXIT();

    /* ─ 3개 문, 5줄에 걸쳐 출력 ─ */
    for (int row = 0; row < 5; ++row) {
        for (int d = 1; d <= 3; ++d) {
            if (d == 1) send_string("   ");
            send_string(doorArt[doorSnap[d]][row]);
            send_string("       "); /* 문 간 간격 */
        }
        send_string("\r\n");
    }

    for (uint8_t i = 1; i <= 3; i++) {
        char lbl[32];
        if (i == 1)
            sprintf(lbl, "       %u", i);
        else
            sprintf(lbl, "               %u", i);
        send_string(lbl);
    }
    send_string("\r\n");

    if (phaseSnap == PHASE_REVEAL || phaseSnap == PHASE_RESULT) {
        for (uint8_t i = 1; i <= 3; i++) {
            uint8_t markDoor = (phaseSnap == PHASE_REVEAL) ? userSnap
                                                           : finalSnap;

            if (i == 1)
                send_string(i == markDoor ? "       ▲" : "        ");
            else
                send_string(i == markDoor ? "               ▲" : "                ");
        }
        send_string("\r\n");
    } else {
        send_string(" \r\n");
    }

    /* ─ 커서(★) 줄 ───────────────────────────────────── */
    for (uint8_t i = 1; i <= 3; i++) {
        bool showStar = false;

        if (phaseSnap == PHASE_SELECT) {
            showStar = (i == cursorDoor); /* 선택 단계 */
        } else if (phaseSnap == PHASE_REVEAL) {
            /* 열리지 않은 두 문 중 하나에만 커서 */
            uint8_t altDoor = 6 - userSnap - hostSnap; /* 남은 다른 문 */
            uint8_t curDoor = (cursorSwitch ? altDoor : userSnap);
            showStar = (i == curDoor);
        }

        if (i == 1)
            send_string(showStar ? "       ★" : "        ");
        else
            send_string(showStar ? "               ★  " : "                ");
    }
    send_string("\r\n\r\n"); /* 끝난 뒤 공백 줄 */

    /* ─ 통계 ─ */
    MakeStatsLine(line, sizeof line);
    send_string(line);
    send_string("\r\n\r\n");

    /* ─ 하단 안내 ─ */
    send_string(footerSnap);
    send_string("\r\n");
}

/*-------------------------------------------------------------*/
/*  RNG 모듈 초기화                                               */
/*-------------------------------------------------------------*/
static void RNG_HwInit(void) {
    /* AHB2 클록 공급 */
    RCC_AHB2PeriphClockCmd(RCC_AHB2Periph_RNG, ENABLE);

    RNG_Cmd(DISABLE);
    RNG_Cmd(ENABLE); /* Enable RNG */

    /* 첫 DRDY 플래그 대기 */
    while (RNG_GetFlagStatus(RNG_FLAG_DRDY) == RESET);
}

/* 32-bit 난수 얻기 */
static inline uint32_t RNG_GetRandom32(void) {
    while (RNG_GetFlagStatus(RNG_FLAG_DRDY) == RESET);
    return RNG_GetRandomNumber();
}

void STM_Nucleo_COMInit(COM_TypeDef COM, USART_InitTypeDef *USART_InitStruct) {
    GPIO_InitTypeDef GPIO_InitStructure;

    /* Enable GPIO clock */
    RCC_AHB1PeriphClockCmd(COM_TX_PORT_CLK[COM] | COM_RX_PORT_CLK[COM],
                           ENABLE);

    if (COM == COM1) {
        /* Enable UART clock */
        RCC_APB1PeriphClockCmd(COM_USART_CLK[COM], ENABLE);
    }

    /* Connect Pxx to USARTx_Tx */
    GPIO_PinAFConfig(COM_TX_PORT[COM],
                     COM_TX_PIN_SOURCE[COM],
                     COM_TX_AF[COM]);

    /* Connect Pxx to USARTx_Rx */
    GPIO_PinAFConfig(COM_RX_PORT[COM],
                     COM_RX_PIN_SOURCE[COM],
                     COM_RX_AF[COM]);

    /* Configure USART Tx as alternate function */
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;

    GPIO_InitStructure.GPIO_Pin = COM_TX_PIN[COM];
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(COM_TX_PORT[COM], &GPIO_InitStructure);

    /* Configure USART Rx as alternate function */
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Pin = COM_RX_PIN[COM];
    GPIO_Init(COM_RX_PORT[COM], &GPIO_InitStructure);

    /* USART configuration */
    USART_Init(COM_USART[COM], USART_InitStruct);

    /* Enable USART */
    USART_Cmd(COM_USART[COM], ENABLE);
}

static void USART_Config(void) {
    USART_InitTypeDef USART_InitStructure;
    USART_InitStructure.USART_BaudRate = 115200;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    STM_Nucleo_COMInit(COM1, &USART_InitStructure);
}

int main(void) {
    OS_ERR err;

    /* Basic Init */
    RCC_DeInit();
    // SystemCoreClockUpdate();
    Setup_Gpio();
    Setup_InputHw();
    /* BSP Init */
    BSP_IntDisAll(); /* Disable all interrupts.                              */
    CPU_Init();      /* Initialize the uC/CPU Services                       */
    Mem_Init();      /* Initialize Memory Management Module                  */
    Math_Init();     /* Initialize Mathematical Module                       */

    /* OS Init */
    OSInit(&err); /* Init uC/OS-III.                                      */

    OSTaskCreate((OS_TCB *)&AppTaskStartTCB, /* Create the start task                                */
                 (CPU_CHAR *)"App Task Start",
                 (OS_TASK_PTR)AppTaskStart,
                 (void *)0u,
                 (OS_PRIO)APP_CFG_TASK_START_PRIO,
                 (CPU_STK *)&AppTaskStartStk[0u],
                 (CPU_STK_SIZE)AppTaskStartStk[APP_CFG_TASK_START_STK_SIZE / 10u],
                 (CPU_STK_SIZE)APP_CFG_TASK_START_STK_SIZE,
                 (OS_MSG_QTY)0u,
                 (OS_TICK)0u,
                 (void *)0u,
                 (OS_OPT)(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
                 (OS_ERR *)&err);

    OSStart(&err); /* Start multitasking (i.e. give control to uC/OS-III). */

    (void)&err;

    return (0u);
}
/*
*********************************************************************************************************
*                                          STARTUP TASK
*
* Description : This is an example of a startup task.  As mentioned in the book's text, you MUST
*               initialize the ticker only once multitasking has started.
*
* Arguments   : p_arg   is the argument passed to 'AppTaskStart()' by 'OSTaskCreate()'.
*
* Returns     : none
*
* Notes       : 1) The first line of code is used to prevent a compiler warning because 'p_arg' is not
*                  used.  The compiler should not generate any code for this statement.
*********************************************************************************************************
*/
static void AppTaskStart(void *p_arg) {
    OS_ERR err;

    (void)p_arg;

    BSP_Init();      /* Initialize BSP functions                             */
    BSP_Tick_Init(); /* Initialize Tick Services.                            */
    USART_Config();
    RNG_HwInit(); /* <<< 추가 */

#if OS_CFG_STAT_TASK_EN > 0u
    OSStatTaskCPUUsageInit(&err); /* Compute CPU capacity with no task running            */
#endif

#ifdef CPU_CFG_INT_DIS_MEAS_EN
    CPU_IntDisMeasMaxCurReset();
#endif

    // BSP_LED_Off(0u);                                            /* Turn Off LEDs after initialization                   */

    APP_TRACE_DBG(("Creating Application Kernel Objects\n\r"));
    AppObjCreate(); /* Create Applicaiton kernel objects                    */

    APP_TRACE_DBG(("Creating Application Tasks\n\r"));
    AppTaskCreate(); /* Create Application tasks                            */
}

static JoyDir_t Joystick_ReadDir(void) {
    ADC_SoftwareStartConv(ADC1);
    while (!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC));
    uint16_t v = ADC_GetConversionValue(ADC1); /* 0-4095 */

    const uint16_t DEAD = 200; /* 중앙 ±200 */
    if (v < 2048 - DEAD)
        return JOY_LEFT;
    else if (v > 2048 + DEAD)
        return JOY_RIGHT;
    else
        return JOY_IDLE;
}

void send_string(const char *str) {
    while (*str) {
        while (USART_GetFlagStatus(Nucleo_COM1, USART_FLAG_TXE) == RESET);
        USART_SendData(Nucleo_COM1, *str++);
    }
}

static void AppTask_LED(void *p_arg) {
    OS_ERR err;
    (void)p_arg;

    for (;;) {
        /* ① 게임 로직이 Sem_LedShow 를 포스트할 때까지 대기 */
        OSSemPend(&Sem_LedShow, 0u, OS_OPT_PEND_BLOCKING, NULL, &err);

        CPU_SR_ALLOC();
        OS_CRITICAL_ENTER(); /* ▼ gameWin 스냅샷 */
        bool win = gameWin;
        OS_CRITICAL_EXIT(); /* ▲ */

        if (win) { /* 승리 → GREEN */
            GPIO_SetBits(GPIOB, LED_GREEN_PIN);
        } else { /* 패배 → RED   */
            GPIO_SetBits(GPIOB, LED_RED_PIN);
        }

        /* ② 2 초 점등 */
        OSTimeDlyHMSM(0, 0, 2, 0, OS_OPT_TIME_HMSM_STRICT, &err);

        /* ③ 소등 */
        GPIO_ResetBits(GPIOB, LED_GREEN_PIN | LED_RED_PIN);
    }
}

static void AppTask_INPUT(void *p_arg) {
    OS_ERR err;
    (void)p_arg;

    bool btnPrev = true; /* 풀-업 → HIGH(1) */
    JoyDir_t dirPrev = JOY_IDLE;

    for (;;) {
        /* ───── ① 조이스틱 이동 처리 ─────────────────────── */
        JoyDir_t dir = Joystick_ReadDir();

        if (dir != dirPrev && dir != JOY_IDLE) {
            CPU_SR_ALLOC();
            OS_CRITICAL_ENTER(); /* ▼ 보호 시작 */
            if (gamePhase == PHASE_SELECT) {
                cursorDoor = (dir == JOY_LEFT)
                                 ? ((cursorDoor == 1) ? 3 : cursorDoor - 1)
                                 : ((cursorDoor == 3) ? 1 : cursorDoor + 1);
            } else if (gamePhase == PHASE_REVEAL) {
                cursorSwitch ^= 1;
            }
            OS_CRITICAL_EXIT(); /* ▲ 보호 끝   */

            /* 세마포어 포스트는 보호 밖에서 */
            OS_CRITICAL_ENTER();
            GamePhase_t phaseSnap = gamePhase;
            OS_CRITICAL_EXIT();
            if (phaseSnap == PHASE_SELECT)
                OSSemPost(&Sem_DisplaySelectDone, OS_OPT_POST_1, &err);
            else if (phaseSnap == PHASE_REVEAL)
                OSSemPost(&Sem_DisplaySwitchDone, OS_OPT_POST_1, &err);
        }
        dirPrev = dir;

        /* ───── ② 확인 버튼 처리 ───────────────────────── */
        bool btnNow = GPIO_ReadInputDataBit(GPIOF, GPIO_Pin_13);
        if (!btnNow && btnPrev) { /* Edge ↓ */
            CPU_SR_ALLOC();
            OS_CRITICAL_ENTER(); /* ▼ */

            if (gamePhase == PHASE_SELECT) {
                userChoice = cursorDoor;
                OS_CRITICAL_EXIT(); /* ▲ */
                OSSemPost(&Sem_UserSelectDone, OS_OPT_POST_1, &err);
                OSSemPost(&Sem_DisplaySelectDone, OS_OPT_POST_1, &err);
            } else if (gamePhase == PHASE_REVEAL) {
                switchChoice = (cursorSwitch == 1);
                OS_CRITICAL_EXIT(); /* ▲ */
                OSSemPost(&Sem_SwitchSelectDone, OS_OPT_POST_1, &err);
                OSSemPost(&Sem_DisplaySwitchDone, OS_OPT_POST_1, &err);
            } else if (gamePhase == PHASE_RESULT) {
                OS_CRITICAL_EXIT(); /* ▲ */
                OSSemPost(&Sem_NextRoundDisp, OS_OPT_POST_1, &err);
                OSSemPost(&Sem_NextRoundLogic, OS_OPT_POST_1, &err);
            }
        }
        btnPrev = btnNow;

        OSTimeDlyHMSM(0, 0, 0, 10, OS_OPT_TIME_HMSM_STRICT, &err); /* 10 ms */
    }
}

/*
*********************************************************************************************************
*                                          AppTaskCreate()
*
* Description : Create application tasks.
*
* Argument(s) : none
*
* Return(s)   : none
*
* Caller(s)   : AppTaskStart()
*
* Note(s)     : none.
*********************************************************************************************************
*/

static void AppTaskCreate(void) {
    OS_ERR err;

    OSTaskCreate(
        (OS_TCB *)&Task_INPUT_TCB,
        (CPU_CHAR *)"AppTask_INPUT",
        (OS_TASK_PTR)AppTask_INPUT,
        (void *)0u,
        (OS_PRIO)0u,
        (CPU_STK *)&Task_INPUT_Stack[0u],
        (CPU_STK_SIZE)Task_INPUT_Stack[(APP_CFG_TASK_START_STK_SIZE * 10) / 10u],
        (CPU_STK_SIZE)APP_CFG_TASK_START_STK_SIZE * 10,
        (OS_MSG_QTY)0u,
        (OS_TICK)0u,
        (void *)0u,
        (OS_OPT)(OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR),
        (OS_ERR *)&err);

    /* GAME Task ----------------------------------------------- */
    OSTaskCreate(&Task_GAME_TCB, "AppTask_GAME",
                 AppTask_GAME, 0,
                 4u, &Task_GAME_Stack[0],
                 APP_CFG_TASK_START_STK_SIZE / 10u,
                 APP_CFG_TASK_START_STK_SIZE * 10,
                 0, 0, 0,
                 OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR, &err);

    /* GameLogic Task: 입력 이벤트 받아 시뮬레이션 수행 */
    OSTaskCreate(&Task_GameLogic_TCB,
                 "AppTask_GameLogic",
                 AppTask_GameLogic,
                 0u,
                 3u, /* 우선순위 조정 */
                 &Task_GameLogic_Stack[0],
                 APP_CFG_TASK_START_STK_SIZE / 10u,
                 APP_CFG_TASK_START_STK_SIZE,
                 0u, 0u, 0u,
                 OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR,
                 &err);

    OSTaskCreate(&Task_LED_TCB, "AppTask_LED",
                 AppTask_LED, 0,
                 5u, /* 우선순위 : GAME보다 낮게 */
                 &Task_LED_Stk[0],
                 APP_CFG_TASK_START_STK_SIZE / 10u,
                 APP_CFG_TASK_START_STK_SIZE,
                 0, 0, 0,
                 OS_OPT_TASK_STK_CHK | OS_OPT_TASK_STK_CLR, &err);
}

/*
*********************************************************************************************************
*                                          AppObjCreate()
*
* Description : Create application kernel objects tasks.
*
* Argument(s) : none
*
* Return(s)   : none
*
* Caller(s)   : AppTaskStart()
*
* Note(s)     : none.
*********************************************************************************************************
*/

static void AppObjCreate(void) {
    OS_ERR err;
    OSSemCreate(&Sem_UserSelectDone, "UserSelDone", 0u, &err);
    OSSemCreate(&Sem_DisplaySelectDone, "DispSelDone", 0u, &err);
    OSSemCreate(&Sem_SwitchSelectDone, "SwitchSelDone", 0u, &err);
    OSSemCreate(&Sem_DisplaySwitchDone, "DispSwitchDone", 0u, &err);
    OSSemCreate(&Sem_ResultReady, "ResultReady", 0u, &err);
    OSSemCreate(&Sem_LedShow, "LedShow", 0u, &err);
    OSSemCreate(&Sem_NextRoundDisp, "NextRoundDisp", 0u, &err);
    OSSemCreate(&Sem_NextRoundLogic, "NextRoundLogic", 0u, &err);
}

/*
*********************************************************************************************************
*                                          Setup_Gpio()
*
* Description : Configure LED GPIOs directly
*
* Argument(s) : none
*
* Return(s)   : none
*
* Caller(s)   : AppTaskStart()
*
* Note(s)     :
*              LED1 PB0
*              LED2 PB7
*              LED3 PB14
*
*********************************************************************************************************
*/
static void Setup_Gpio(void) {
    GPIO_InitTypeDef led_init = {0};

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_AHB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);

    led_init.GPIO_Mode = GPIO_Mode_OUT;
    led_init.GPIO_OType = GPIO_OType_PP;
    led_init.GPIO_Speed = GPIO_Speed_2MHz;
    led_init.GPIO_PuPd = GPIO_PuPd_NOPULL;
    led_init.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_7 | GPIO_Pin_14;

    GPIO_Init(GPIOB, &led_init);
}
/*-------------------------------------------------------------*/
/*  AppTask_GAME : CLI 기반 Monty-Hall 화면 갱신               */
/*-------------------------------------------------------------*/
static void AppTask_GAME(void *p_arg) {
    OS_ERR err;

    for (;;) {
        /* 선택 단계 ------------------------------------------------ */
        CPU_SR_ALLOC();
        OS_CRITICAL_ENTER();
        uint8_t doorSnap = cursorDoor;
        GamePhase_t phase = gamePhase;
        OS_CRITICAL_EXIT();
        RenderScreen(doorSnap, 0);
        while (phase == PHASE_SELECT) {
            OSSemPend(&Sem_DisplaySelectDone, 0, OS_OPT_PEND_BLOCKING, NULL, &err);
            CPU_SR_ALLOC();
            OS_CRITICAL_ENTER();
            doorSnap = cursorDoor;
            phase = gamePhase;
            OS_CRITICAL_EXIT();
            RenderScreen(doorSnap, 0);
        }

        /* 스테이/스위치 단계 --------------------------------------- */
        OS_CRITICAL_ENTER();
        doorSnap = cursorDoor;
        uint8_t switchSnap = cursorSwitch;
        phase = gamePhase;
        OS_CRITICAL_EXIT();
        RenderScreen(doorSnap, switchSnap);
        while (phase == PHASE_REVEAL) {
            OSSemPend(&Sem_DisplaySwitchDone, 0, OS_OPT_PEND_BLOCKING, NULL, &err);
            CPU_SR_ALLOC();
            OS_CRITICAL_ENTER();
            doorSnap = cursorDoor;
            switchSnap = cursorSwitch;
            phase = gamePhase;
            OS_CRITICAL_EXIT();
            RenderScreen(doorSnap, switchSnap);
        }

        /* 결과 단계 ----------------------------------------------- */
        OSSemPend(&Sem_ResultReady, 0, OS_OPT_PEND_BLOCKING, NULL, &err);
        RenderScreen(0, 0);                           /* 문 + 결과 표시 */
        OSSemPost(&Sem_LedShow, OS_OPT_POST_1, &err); /* 2 s LED */

        /* BTN 누를 때까지 대기 */
        OSSemPend(&Sem_NextRoundDisp, 0, OS_OPT_PEND_BLOCKING, NULL, &err);
    }
}

static void AppTask_GameLogic(void *p_arg) {
    OS_ERR err;

    for (;;) {
        OSSemSet(&Sem_DisplaySelectDone, 0, &err);
        OSSemSet(&Sem_DisplaySwitchDone, 0, &err);
        /* 1) 라운드 초기화 */
        CPU_SR_ALLOC();
        OS_CRITICAL_ENTER();
        cursorDoor = 1;
        cursorSwitch = 0;
        prizeDoor = (RNG_GetRandom32() % 3) + 1;
        gamePhase = PHASE_SELECT;
        userChoice = 0;
        switchChoice = false;
        doors[1] = doors[2] = doors[3] = DOOR_CLOSED;
        strcpy(footer, "←/→ to move, BTN select");
        OS_CRITICAL_EXIT();

        OSSemPost(&Sem_DisplaySelectDone, OS_OPT_POST_1, &err); /* 화면 갱신 요청 */

        /* 2) 사용자 첫 선택 대기 */
        OSSemPend(&Sem_UserSelectDone, 0u, OS_OPT_PEND_BLOCKING, NULL, &err);
        /* 3) 호스트 문 공개 (userChoice, prizeDoor 기반) */
        OS_CRITICAL_ENTER();
        if (userChoice == prizeDoor) {
            // 상금 아닌 문 중 랜덤 공개
            do {
                hostChoice = (RNG_GetRandom32() % 3) + 1;
            } while (hostChoice == prizeDoor);
        } else {
            // 남은 하나(상금도 아님) 공개
            for (uint8_t d = 1; d <= 3; d++) {
                if (d != userChoice && d != prizeDoor) {
                    hostChoice = d;
                    break;
                }
            }
        }

        /* 4) 교체 여부 선택 단계로 전환 */
        gamePhase = PHASE_REVEAL;
        cursorSwitch = 0;
        /* 5) 사용자 교체/유지 선택 대기 */
        /* 호스트가 염소 문을 연 직후 ---------------------------- */
        doors[hostChoice] = DOOR_OPEN_GOAT;
        strcpy(footer, "←/→ Toggle Stay/Switch, BTN confirm");
        OS_CRITICAL_EXIT();
        OSSemPost(&Sem_DisplaySwitchDone, OS_OPT_POST_1, &err); /* 화면 갱신 요청 */
        OSSemPend(&Sem_SwitchSelectDone, 0u, OS_OPT_PEND_BLOCKING, NULL, &err);

        /* 6) 최종 선택 결정 */
        OS_CRITICAL_ENTER();
        uint8_t finalDoor = switchChoice
                                ? (uint8_t)(6 - userChoice - hostChoice)  // 1+2+3=6
                                : userChoice;

        finalDoorChoice = finalDoor;
        gameWin = (finalDoor == prizeDoor);
        gamePhase = PHASE_RESULT;
        /* ─ 통계 누적 ------------------------------------------- */
        g_roundCount++;
        if (gameWin)
            g_winCount++;
        else
            g_loseCount++;
        /* 결과 확정 직후 --------------------------------------- */
        doors[prizeDoor] = DOOR_OPEN_PRIZE;
        if (!gameWin)
            doors[finalDoor] = DOOR_OPEN_FAIL; /* 최종 선택 문을 FAIL 상태로 */
        else {
            /* 승리 → 나머지 한 문을 EMPTY 로 열어 줌 */
            uint8_t remDoor = 6 - prizeDoor - hostChoice; /* 세 문 합 1 + 2 + 3 = 6 */
            doors[remDoor] = DOOR_OPEN_FAIL;
        }

        strcpy(footer, gameWin ? "\033[32mWIN!\033[0m – press BTN for next round"
                               : "\033[31mLOSE!\033[0m – press BTN for next round");
        OS_CRITICAL_EXIT();
        OSSemPost(&Sem_ResultReady, OS_OPT_POST_1, &err); /* 화면 갱신 요청 */

        /* 7) 잠시 대기 후 재시작 */
        // OSTimeDlyHMSM(0, 0, 5, 0, OS_OPT_TIME_HMSM_STRICT, &err);
        OSSemPend(&Sem_NextRoundLogic, 0u,
                  OS_OPT_PEND_BLOCKING, NULL, &err);
    }
}

static void Setup_InputHw(void) {
    /* 버튼 PF13 -------------------------------------------------- */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOF, ENABLE);
    GPIO_InitTypeDef gpio_btn = {
        .GPIO_Pin = GPIO_Pin_13,
        .GPIO_Mode = GPIO_Mode_IN,
        .GPIO_PuPd = GPIO_PuPd_UP};
    GPIO_Init(GPIOF, &gpio_btn);

    /* 조이스틱 PC0 (ADC1_IN10) ---------------------------------- */
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    GPIO_InitTypeDef gpio_adc = {
        .GPIO_Pin = GPIO_Pin_0,
        .GPIO_Mode = GPIO_Mode_AN,
        .GPIO_PuPd = GPIO_PuPd_NOPULL};
    GPIO_Init(GPIOC, &gpio_adc);

    ADC_InitTypeDef adc = {
        .ADC_Resolution = ADC_Resolution_12b,
        .ADC_ContinuousConvMode = DISABLE,
        .ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None,
        .ADC_DataAlign = ADC_DataAlign_Right,
        .ADC_NbrOfConversion = 1};
    ADC_Init(ADC1, &adc);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_10, 1, ADC_SampleTime_84Cycles);
    ADC_Cmd(ADC1, ENABLE);
}
