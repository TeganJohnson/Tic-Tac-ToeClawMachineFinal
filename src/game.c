#include "game.h"
#include "display.h"
#include "motor.h"
#include <stdint.h>

// -----------------------------------------------------------------------------
// External functions from other files
// -----------------------------------------------------------------------------
extern uint32_t millis(void);
extern void delay_ms(uint32_t ms);

extern void Joystick_Read(uint16_t *x, uint16_t *y, uint8_t *pressed);
extern void Hardware_ScanBoard(uint8_t scanned_board[9]);

extern void Display_ShowIdleScreen(void);
extern void Display_ShowPlayerTurn_Grab(player_t player, uint32_t time_remaining_ms, uint8_t bg);
extern void Display_ShowPlayerTurn_Drop(player_t player, uint32_t time_remaining_ms, uint8_t bg);
extern void Display_ShowCheckingBoard(void);
extern void Display_ShowWinner(player_t winner);
extern void Display_ShowDraw(void);

extern void Motor_Step(uint8_t axis, uint8_t dir, uint32_t steps);
extern void Display_Current_Timer(uint32_t time_remaining_ms);

// If these already exist in another file, keep these as externs.
// If not, replace them with static stub implementations instead.
extern void Claw_Grab_Token(void);
extern void Claw_Drop_Token(void);

#define Z_LIMIT_GPIO    GPIOB
#define Z_LIMIT_PIN     7

// -----------------------------------------------------------------------------
// Hardware / movement definitions
// -----------------------------------------------------------------------------
#define X_LIMIT_GPIO GPIOB
#define X_LIMIT_PIN  3

#define Y_LIMIT_GPIO GPIOB
#define Y_LIMIT_PIN 4

#ifndef AXIS_X
#define AXIS_X 0
#endif

#ifndef AXIS_Y
#define AXIS_Y 1
#endif

#ifndef DIR_FORWARD
#define DIR_FORWARD 0
#endif

#ifndef DIR_BACKWARD
#define DIR_BACKWARD 1
#endif

// -----------------------------------------------------------------------------
// Static game context
// -----------------------------------------------------------------------------
static game_context_t game;

// -----------------------------------------------------------------------------
// Winning line lookup table
// -----------------------------------------------------------------------------
static const uint8_t win_lines[8][3] = {
    {0, 1, 2},
    {3, 4, 5},
    {6, 7, 8},
    {0, 3, 6},
    {1, 4, 7},
    {2, 5, 8},
    {0, 4, 8},
    {2, 4, 6}
};

// -----------------------------------------------------------------------------
// State / input tracking
// -----------------------------------------------------------------------------
static uint8_t Idle_IsDisplayed = 0;
static uint8_t remaining_prev_sec = 0xFF;
static uint8_t button_was_down = 1;   // start latched to avoid boot glitches

// -----------------------------------------------------------------------------
// Limit and direction tracking
// -----------------------------------------------------------------------------
static uint8_t xdir = 0;
static uint8_t ydir = 0;
static uint8_t xlim = LIM_NONE;
static uint8_t ylim = LIM_NONE;

static uint32_t PLAYER_TURN_TIME_MS = 30000;

// -----------------------------------------------------------------------------
// Board helpers
// -----------------------------------------------------------------------------
static void Board_Init(void)
{
    for (uint8_t i = 0; i < 9; i++) {
        game.board[i] = PLAYER_NONE;
    }

    game.move_count = 0;
    game.last_move_cell = 0xFF;
}

static void Board_RecountMoves(void)
{
    uint8_t count = 0;
    uint8_t last = 0xFF;

    for (uint8_t i = 0; i < 9; i++) {
        if (game.board[i] != PLAYER_NONE) {
            count++;
            last = i;
        }
    }

    game.move_count = count;
    game.last_move_cell = last;
}

static player_t Board_CheckWinner(void)
{
    for (uint8_t line = 0; line < 8; line++) {
        uint8_t a = win_lines[line][0];
        uint8_t b = win_lines[line][1];
        uint8_t c = win_lines[line][2];

        if (game.board[a] != PLAYER_NONE &&
            game.board[a] == game.board[b] &&
            game.board[b] == game.board[c]) {
            return (player_t)game.board[a];
        }
    }

    return PLAYER_NONE;
}

static uint8_t Board_IsDraw(void)
{
    return (game.move_count >= 9 && Board_CheckWinner() == PLAYER_NONE);
}

static void Board_ScanAllCells(uint8_t scanned_board[9])
{
    Hardware_ScanBoard(scanned_board);
}

static void Board_UpdateFromScan(void)
{
    uint8_t scanned_board[9];

    Board_ScanAllCells(scanned_board);

    for (uint8_t i = 0; i < 9; i++) {
        game.board[i] = scanned_board[i];
    }

    Board_RecountMoves();
}


// -----------------------------------------------------------------------------
// Motion helper
// -----------------------------------------------------------------------------
static void Claw_UpdateFromJoystick(uint16_t x, uint16_t y)
{
    X_Limit_Checker(xdir, &xlim);
    Y_Limit_Checker(ydir, &ylim);

    if (x > y && x > 3000 && xlim != LIM_POS) {
        Motor_Step(AXIS_X, DIR_FORWARD, 10);
        xdir = DIR_FORWARD;
    }
    else if (y > x && y > 3000 && ylim != LIM_POS) {
        Motor_Step(AXIS_Y, DIR_FORWARD, 10);
        ydir = DIR_FORWARD;
    }
    else if (x < y && x < 1500 && xlim != LIM_NEG) {
        Motor_Step(AXIS_X, DIR_BACKWARD, 10);
        xdir = DIR_BACKWARD;
    }
    else if (y < x && y < 1500 && ylim != LIM_NEG) {
        Motor_Step(AXIS_Y, DIR_BACKWARD, 10);
        ydir = DIR_BACKWARD;
    }
}

// -----------------------------------------------------------------------------
// Button helper: detects only a fresh press edge
// -----------------------------------------------------------------------------
static uint8_t Button_JustPressed(void)
{
    uint16_t jx, jy;
    uint8_t pressed;


    delay_ms(20);  // simple debounce
    Joystick_Read(&jx, &jy, &pressed);

    if (pressed && !button_was_down) {
        button_was_down = 1;
        return 1;
    }

    if (!pressed) {
        button_was_down = 0;
    }

    return 0;
}

static void Button_RefreshLatch(void)
{
    uint16_t jx, jy;
    uint8_t pressed;

    Joystick_Read(&jx, &jy, &pressed);
    button_was_down = pressed;
}

// -----------------------------------------------------------------------------
// State transition helper
// -----------------------------------------------------------------------------
static void Game_ChangeState(game_state_t new_state)
{
    game.current_state = new_state;
    game.turn_start_time = millis();

    switch (new_state)
    {
    case STATE_IDLE:
        Idle_IsDisplayed = 0;
        remaining_prev_sec = 0xFF;
        button_was_down = 1;
        break;

    case STATE_PLAYER1_TURN_GRAB:
        remaining_prev_sec = 0xFF;
        game.active_player = PLAYER_1;
        button_was_down = 1;   // require release before next press is accepted
        Display_ShowPlayerTurn_Grab(PLAYER_1, PLAYER_TURN_TIME_MS, 1);
        break;

    case STATE_PLAYER2_TURN_GRAB:
        remaining_prev_sec = 0xFF;
        game.active_player = PLAYER_2;
        button_was_down = 1;
        Display_ShowPlayerTurn_Grab(PLAYER_2, PLAYER_TURN_TIME_MS, 1);
        break;

    case STATE_PLAYER1_TURN_DROP:
        remaining_prev_sec = 0xFF;
        game.active_player = PLAYER_1;
        button_was_down = 1;
        Display_ShowPlayerTurn_Drop(PLAYER_1, PLAYER_TURN_TIME_MS, 1);
        break;

    case STATE_PLAYER2_TURN_DROP:
        remaining_prev_sec = 0xFF;
        game.active_player = PLAYER_2;
        button_was_down = 1;
        Display_ShowPlayerTurn_Drop(PLAYER_2, PLAYER_TURN_TIME_MS, 1);
        break;

    case STATE_CHECK_BOARD:
        remaining_prev_sec = 0xFF;
        button_was_down = 1;
        Display_ShowCheckingBoard();
        break;

    case STATE_PLAYER1_WIN:
        button_was_down = 1;
        Display_ShowWinner(PLAYER_1);
        break;

    case STATE_PLAYER2_WIN:
        button_was_down = 1;
        Display_ShowWinner(PLAYER_2);
        break;

    case STATE_DRAW:
        button_was_down = 1;
        Display_ShowDraw();
        break;

    case STATE_RESET:
    default:
        break;
    }
}

static void Update_Timer(uint8_t direction)
{
    if (direction == 1) {
        if (PLAYER_TURN_TIME_MS < 60000U) {
            PLAYER_TURN_TIME_MS += 15000U;

            if (PLAYER_TURN_TIME_MS > 60000U) {
                PLAYER_TURN_TIME_MS = 60000U;
            }
        }
    } else {
        if (PLAYER_TURN_TIME_MS > 15000U) {
            PLAYER_TURN_TIME_MS -= 15000U;

            if (PLAYER_TURN_TIME_MS < 15000U) {
                PLAYER_TURN_TIME_MS = 15000U;
            }
        }
    }

    Display_Current_Timer(PLAYER_TURN_TIME_MS);
}

// -----------------------------------------------------------------------------
// State handlers
// -----------------------------------------------------------------------------
static void Handle_IdleState(void)
{
    uint16_t x, y;
    uint8_t pressed;


    if (!Idle_IsDisplayed) {
        Display_ShowIdleScreen();
        Display_Current_Timer(PLAYER_TURN_TIME_MS);
        Idle_IsDisplayed = 1;
    }

    if (Button_JustPressed()) {
        Board_Init();
        game.active_player = PLAYER_1;
        Game_ChangeState(STATE_PLAYER1_TURN_GRAB);
    }

    Joystick_Read(&x, &y, &pressed);

    if (y > 3000) {
        Update_Timer(1);
        delay_ms(300);
    }
    else if (y < 1500) {
        Update_Timer(0);
        delay_ms(300);
    }
}



static void Handle_PlayerTurnState_Grab(void)
{
    uint32_t elapsed = millis() - game.turn_start_time;
    uint32_t remaining =
        (elapsed >= PLAYER_TURN_TIME_MS) ? 0 : (PLAYER_TURN_TIME_MS - elapsed);

    uint8_t remaining_sec = (uint8_t)((remaining + 999) / 1000);

    uint16_t jx, jy;
    uint8_t pressed;

    Joystick_Read(&jx, &jy, &pressed);
    Claw_UpdateFromJoystick(jx, jy);

    if (remaining_sec != remaining_prev_sec) {
        Display_ShowPlayerTurn_Grab(game.active_player, remaining, 0);
        remaining_prev_sec = remaining_sec;
    }

    if (pressed && !button_was_down) {
        button_was_down = 1;
        Claw_Grab_Token();

        if (game.active_player == PLAYER_1) {
            Game_ChangeState(STATE_PLAYER1_TURN_DROP);
        } else {
            Game_ChangeState(STATE_PLAYER2_TURN_DROP);
        }
        return;
    }

    if (!pressed) {
        button_was_down = 0;
    }

    if (elapsed >= PLAYER_TURN_TIME_MS) {
        Claw_Grab_Token();

        if (game.active_player == PLAYER_1) {
            Game_ChangeState(STATE_PLAYER1_TURN_DROP);
        } else {
            Game_ChangeState(STATE_PLAYER2_TURN_DROP);
        }
        return;
    }
}

static void Handle_PlayerTurnState_Drop(void)
{
    uint32_t elapsed = millis() - game.turn_start_time;
    uint32_t remaining =
        (elapsed >= PLAYER_TURN_TIME_MS) ? 0 : (PLAYER_TURN_TIME_MS - elapsed);

    uint8_t remaining_sec = (uint8_t)((remaining + 999) / 1000);

    uint16_t jx, jy;
    uint8_t pressed;

    Joystick_Read(&jx, &jy, &pressed);
    Claw_UpdateFromJoystick(jx, jy);

    if (remaining_sec != remaining_prev_sec) {
        Display_ShowPlayerTurn_Drop(game.active_player, remaining, 0);
        remaining_prev_sec = remaining_sec;
    }

    if (pressed && !button_was_down) {
        button_was_down = 1;
        Claw_Drop_Token();
        Game_ChangeState(STATE_CHECK_BOARD);
        return;
    }

    if (!pressed) {
        button_was_down = 0;
    }

    if (elapsed >= PLAYER_TURN_TIME_MS) {
        Claw_Drop_Token();
        Game_ChangeState(STATE_CHECK_BOARD);
        return;
    }
}

static void Handle_CheckBoardState(void)
{
    Board_UpdateFromScan();

    player_t winner = Board_CheckWinner();

    if (winner == PLAYER_1) {
        Game_ChangeState(STATE_PLAYER1_WIN);
        return;
    }

    if (winner == PLAYER_2) {
        Game_ChangeState(STATE_PLAYER2_WIN);
        return;
    }

    if (Board_IsDraw()) {
        Game_ChangeState(STATE_DRAW);
        return;
    }

    if (game.active_player == PLAYER_1) {
        Game_ChangeState(STATE_PLAYER2_TURN_GRAB);
    } else {
        Game_ChangeState(STATE_PLAYER1_TURN_GRAB);
    }
}

static void Handle_EndState(void)
{
    if ((millis() - game.turn_start_time) >= TRANSITION_TIME_MS) {
        Game_ChangeState(STATE_RESET);
    }
}

static void Handle_ResetState(void)
{
    xlim = LIM_NONE;
    ylim = LIM_NONE;
    xdir = 0;
    ydir = 0;

    Board_Init();
    game.active_player = PLAYER_1;
    Game_ChangeState(STATE_IDLE);

    // prevent immediate restart if button is still being held
    Button_RefreshLatch();
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void Game_Init(void)
{
    Board_Init();
    game.active_player = PLAYER_1;
    game.turn_start_time = 0;
    remaining_prev_sec = 0xFF;
    button_was_down = 1;

    Game_ChangeState(STATE_IDLE);
    Button_RefreshLatch();
}

void Game_Update(void)
{
    switch (game.current_state)
    {
    case STATE_IDLE:
        Handle_IdleState();
        break;

    case STATE_PLAYER1_TURN_GRAB:
    case STATE_PLAYER2_TURN_GRAB:
        Handle_PlayerTurnState_Grab();
        break;

    case STATE_PLAYER1_TURN_DROP:
    case STATE_PLAYER2_TURN_DROP:
        Handle_PlayerTurnState_Drop();
        break;

    case STATE_CHECK_BOARD:
        Handle_CheckBoardState();
        break;

    case STATE_PLAYER1_WIN:
    case STATE_PLAYER2_WIN:
    case STATE_DRAW:
        Handle_EndState();
        break;

    case STATE_RESET:
        Handle_ResetState();
        break;

    default:
        Game_ChangeState(STATE_IDLE);
        break;
    }
}

game_state_t Game_GetState(void)
{
    return game.current_state;
}

player_t Game_GetActivePlayer(void)
{
    return game.active_player;
}

const game_context_t* Game_GetContext(void)
{
    return &game;
}