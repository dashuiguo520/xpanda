// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef INT_MAIL_HPP
#define INT_MAIL_HPP

#include <common/cbasetypes.hpp>
#include <common/timer.hpp>

struct mail_message;

TIMER_FUNC(mail_return_timer);
TIMER_FUNC(mail_delete_timer);

int32 inter_mail_parse_frommap(int32 fd);
bool mail_sendmail(int32 send_id, const char* send_name, int32 dest_id, const char* dest_name, const char* title, const char* body, int32 zeny, struct item *item, int32 amount);
// Joins the caller's open transaction instead of managing its own (A4):
// MySQL implicitly commits on START TRANSACTION, so nesting mail_sendmail
// would commit the auction state flip without its mails (audit P0-2).
bool mail_sendmail_in_tx(int32 send_id, const char* send_name, int32 dest_id, const char* dest_name, const char* title, const char* body, int32 zeny, struct item *item, int32 amount);

int32 inter_mail_sql_init(void);
void inter_mail_sql_final(void);

int32 mail_savemessage(struct mail_message* msg);
void mapif_Mail_new(struct mail_message *msg);

#endif /* INT_MAIL_HPP */
