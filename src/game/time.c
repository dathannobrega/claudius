#include "time.h"

#include <math.h>

static struct {
    int tick; // 50 ticks in a day
    int day; // 16 days in a month
    int month; // 12 months in a year
    int year;
    int total_days;
} data;

void game_time_init(int year)
{
    data.tick = 0;
    data.day = 0;
    data.month = 0;
    data.total_days = 0;
    data.year = year;
}

int game_time_tick(void)
{
    return data.tick;
}

int game_time_day(void)
{
    return data.day;
}

int game_time_month(void)
{
    return data.month;
}

int game_time_year(void)
{
    return data.year;
}

int game_time_advance_tick(void)
{
    if (++data.tick >= GAME_TIME_TICKS_PER_DAY) {
        data.tick = 0;
        return 1;
    }
    return 0;
}

int game_time_advance_day(void)
{
    data.total_days++;
    if (++data.day >= GAME_TIME_DAYS_PER_MONTH) {
        data.day = 0;
        return 1;
    }
    return 0;
}

int game_time_advance_month(void)
{
    if (++data.month >= GAME_TIME_MONTHS_PER_YEAR) {
        data.month = 0;
        return 1;
    }
    return 0;
}

void game_time_advance_year(void)
{
    ++data.year;
}

int game_time_total_months(void)
{
    return data.total_days / GAME_TIME_DAYS_PER_MONTH;
}

int game_time_total_years(void)
{
    return game_time_total_months() / GAME_TIME_MONTHS_PER_YEAR;
}

int game_time_total_days(void)
{
    return data.total_days;
}

void game_time_set_state(int tick, int day, int month, int year, int total_days)
{
    data.tick = tick >= 0 ? tick : 0;
    if (data.tick >= GAME_TIME_TICKS_PER_DAY) {
        data.tick %= GAME_TIME_TICKS_PER_DAY;
    }

    data.day = day >= 0 ? day : 0;
    if (data.day >= GAME_TIME_DAYS_PER_MONTH) {
        data.day %= GAME_TIME_DAYS_PER_MONTH;
    }

    data.month = month >= 0 ? month : 0;
    if (data.month >= GAME_TIME_MONTHS_PER_YEAR) {
        data.month %= GAME_TIME_MONTHS_PER_YEAR;
    }

    data.year = year;
    data.total_days = total_days >= 0 ? total_days : 0;
}

void game_time_save_state(buffer *buf)
{
    buffer_write_i32(buf, data.tick);
    buffer_write_i32(buf, data.day);
    buffer_write_i32(buf, data.month);
    buffer_write_i32(buf, data.year);
    buffer_write_i32(buf, data.total_days);
}

void game_time_load_state(buffer *buf)
{
    data.tick = buffer_read_i32(buf);
    data.day = buffer_read_i32(buf);
    data.month = buffer_read_i32(buf);
    data.year = buffer_read_i32(buf);
    data.total_days = buffer_read_i32(buf);
}

void game_time_load_basic_info(buffer *buf, int *month, int *year)
{
    buffer_skip(buf, 8);
    *month = buffer_read_i32(buf);
    *year = buffer_read_i32(buf);
}
