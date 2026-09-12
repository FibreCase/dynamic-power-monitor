/*
 * net_task.h — the TCP task: owns the socket, the connect state machine,
 * the downstream (host -> ESP32) control frames, and the upstream sample sends.
 * All of its socket state is private to this translation unit.
 */
#pragma once

/* Start the net task. */
void net_task_create(void);
