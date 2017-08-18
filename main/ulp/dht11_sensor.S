/**
 * @file dht11_sensor.S
 * @brief ULP用DHT-11センサ読み取りプログラム
 */
// Copyright 2017 Kenta IDA
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "soc/rtc_cntl_reg.h"
#include "soc/rtc_io_reg.h"
#include "soc/soc_ulp.h"

	.bss
	.global state
state:
	.long 0	// 処理状態

	.global byte_index
byte_index:
	.long 0	// 次に受信するバイト

	.global bit_index
bit_index:
	.long 0	// 次に受信するビット

	.global last_temperature
last_temperature:	// 前回の温度
	.long 0

	.global last_humidity
last_humidity:		// 前回の湿度
	.long 0

	.global force_wakeup_counter
force_wakeup_counter: // 強制的に送信を行うためのカウンタ
	.long 0
	
	.global debug
debug:
	.long 0		// デバッグ用

	.global sensor_data
sensor_data:
	.long 0	//
	.long 0	//
	.long 0	//
	.long 0	//
	.long 0	//

// ULPプログラムのエントリ・ポイント
.set PIN_DATA, (11+14)
.set PIN_TRIGGER, (10+14)

	.text
	.global entry
entry:	// タイマで起こされた場合の処理開始位置
	// 処理状態
	move r0, state
	ld r0, r0, 0
	jumpr state_0, 1, LT
	jumpr state_1, 2, LT
	

state_0:
	move r0, state	// STATE <= STATE_1
	move r1, 1		//
	st r1, r0, 0	// /
	sleep 0			// 0番目のULP起動タイマを選択
	halt			// 0番目のタイマで起こされるまで待つ

state_1:	// STATE_1: センサの起動と0ビット目先頭待ち
	WRITE_RTC_REG(RTC_GPIO_OUT_W1TS_REG, PIN_TRIGGER, 1, 1)
						// デバッグ用トリガ信号をセット
	WRITE_RTC_REG(RTC_GPIO_ENABLE_W1TC_REG, PIN_DATA, 1, 1)	// Release line
	WRITE_RTC_REG(RTC_GPIO_OUT_W1TC_REG, PIN_DATA, 1, 1)	// 
	WRITE_RTC_REG(RTC_GPIO_ENABLE_W1TS_REG, PIN_DATA, 1, 1)	// Start
	
	move r0, 0			// 18[ms]以上ウェイトする
hold_start:
	add r0, r0, 1
	wait 8000	// 8000[cyc] = 1[ms]ウェイト
	jumpr hold_start,30, LT	//とりあえず30[ms]くらい
	//
	WRITE_RTC_REG(RTC_GPIO_OUT_W1TS_REG, PIN_DATA, 1, 1)	// 
	WRITE_RTC_REG(RTC_GPIO_ENABLE_W1TC_REG, PIN_DATA, 1, 1)	// Release line
	
wait_start_rising:	// リリース後 highになるの待つ
	READ_RTC_REG(RTC_GPIO_IN_REG, PIN_DATA, 1)
	and r0, r0, 1
	jump wait_start_rising, eq
wait_start_falling:	// センサのACKの立下り待ち
	READ_RTC_REG(RTC_GPIO_IN_REG, PIN_DATA, 1)
	sub r0, r0, 1
	jump wait_start_falling, eq

	wait 8*120		// センサのACKの3/4くらいまで待つ
wait_bit_0_edge:	// bit0の開始位置まで待つ
	READ_RTC_REG(RTC_GPIO_IN_REG, PIN_DATA, 1)
	sub r0, r0, 1
	jump wait_bit_0_edge, eq



state_2:	// STATE_2: データの読み取り
wait_rising_edge:	// 立ち上がりまで待つ
	READ_RTC_REG(RTC_GPIO_IN_REG, PIN_DATA, 1)
	and r0, r0, 1
	jump wait_rising_edge, eq
	wait 8*50		// 約50[us]待つ

	READ_RTC_REG(RTC_GPIO_IN_REG, PIN_DATA, 1)
	move r2, sensor_data		// r2 <= sensor_data + byte_index
	move r1, byte_index			//
	ld r1, r1, 0				//
	add r2, r2, r1				// /
	ld r1, r2, 0				// r1 <= *sensor_data
	lsh r1, r1, 1				// r1 <<= 1
	or r1, r1, r0				// r1 |= r0
	st r1, r2, 0				// *sensor_data <= r1
	
	move r3, r0					// Save the input data to R3

update_index:
	move r2, bit_index			// bit_indexをインクリメント
	ld r0, r2, 0
	add r0, r0, 1
	st r0, r2, 0				//
	
	jumpr end_read_bit, 8, LT	// bit_index < 8 ならジャンプ
	
	move r0, 0				// bit index = 0
	st r0, r2, 0			// /
	move r2, byte_index
	ld r0, r2, 0
	add r0, r0, 1
	st r0, r2, 0
	jumpr end_read_bit, 5, LT	// byte_index < 5ならジャンプ
		
		// センサデータ読み取り完了
		move r0, 0		//
		st r0, r2, 0	// byte_index = 0

	check_temperature:
		move r3, last_temperature	// 前回値から温度が変化したか
		ld r0, r3, 0
		add r0, r0, 0				// 前回値が0ならチェック省略
		jump do_halt, EQ			// /
		move r3, sensor_data
		ld r1, r3, 8	// オフセット+8bytes: 温度データ
		sub r0, r0, r1
		and r0, r0, 0xff
		jump check_humidity, EQ
		jump do_wakeup

	check_humidity:
		move r3, last_humidity		// 前回値から湿度が変化したか
		ld r0, r3, 0	
		move r3, sensor_data
		ld r1, r3, 0	// オフセット+0bytes: 湿度データ
		sub r0, r0, r1
		and r0, r0, 0xff
		jump check_stage, EQ
		jump do_wakeup

	check_stage:
		move r3, force_wakeup_counter
		ld r0, r3, 0
		add r0, r0, 1
		st r0, r3, 0	// 変化なし回数をインクリメント
		jumpr do_halt, 12, LT	// 12回変化なしが続くまでは何もしない
		move r3, debug
		move r0, 1
		st r0, r3, 0

	do_wakeup:
		move r0, 0
		move r3, force_wakeup_counter
		st r0, r3, 0		// 強制起動カウンタをリセット
						
							// デバッグ用トリガ信号をクリア
		wake				// メインCPUを起こす
							// ULPの起動タイマを無効化
		WRITE_RTC_FIELD(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN, 0)
	do_halt:
		// 変化検出用に、今回のセンサーデータを保存しておく
		move r3, sensor_data
		ld r0, r3, 0	// オフセット+0bytes: 湿度データ
		move r2, last_humidity
		st r0, r2, 0
		ld r0, r3, 8	// オフセット+8bytes: 温度データ
		move r2, last_temperature
		st r0, r2, 0

		move r3, state
		move r0, 1
		st r0, r3, 0	// STATE <= STATE_1

		WRITE_RTC_REG(RTC_GPIO_OUT_W1TC_REG, PIN_TRIGGER, 1, 1)	
		halt				// 処理終了


end_read_bit:			// ビット読み取り後の処理
	and r3, r3, 1		// 読み取ったビットが0ならジャンプ
	jump wait_short, eq
	
wait_falling_edge:		// 1のビットのHIGHの状態なので、立下りまで待つ
	READ_RTC_REG(RTC_GPIO_IN_REG, PIN_DATA, 1)
	sub r0, r0, 1
	jump wait_falling_edge, eq
	jump state_2

wait_short:				// 0のビットなので何もしない。
	//wait 8*80
	jump state_2
