// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Atcommands
 * Place the body of custom atcommands in this file.
 * Format:
 *	ACMD_FUNC(command_name)
 *	{
 *		<code>;
 *	}
 **/

//ACMD_FUNC(newcommand)
//{
//	clif_displaymessage(fd, "It works!");
//	clif_specialeffect(&sd->bl, EF_HEARTCASTING, AREA);
//	return 0;
//}

ACMD_FUNC(suspend) {
	nullpo_retr(-1, sd);

	if (pc_isdead(sd)) {
		//clif_displaymessage(fd, msg_txt_cn(sd, 81)); // You cannot enter suspend mode when dead.
		return -1;
	}

	if (map_flag_vs2(sd->m)) {
		//clif_displaymessage(fd, msg_txt_cn(sd, 82)); // You cannot enter suspend mode on this map.
		return -1;
	}

	suspend_active(sd, SUSPEND_MODE_OFFLINE);
	return 0;
}

/* ===========================================================
 * 指令: afk
 * 描述: 使角色进入离开模式, 角色将会坐到地上并自动使用 AFK 头饰
 * 用法: @afk
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
ACMD_FUNC(afk) {
	nullpo_retr(-1, sd);

	if (pc_isdead(sd)) {
		//clif_displaymessage(fd, msg_txt_cn(sd, 83)); // You cannot enter afk mode when dead.
		return -1;
	}

	if (map_flag_vs2(sd->m)) {
		//clif_displaymessage(fd, msg_txt_cn(sd, 84)); // You cannot enter afk mode on this map.
		return -1;
	}

	suspend_active(sd, SUSPEND_MODE_AFK);
	return 0;
}
