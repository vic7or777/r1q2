/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// cmd.c -- Quake script command processing module

#include "qcommon.h"

void Cmd_ForwardToServer (void);

cmdalias_t	*cmd_alias;

qboolean	cmd_wait;

#define	ALIAS_LOOP_COUNT	16
int		alias_count;		// for detecting runaway loops

#ifndef DEDICATED_ONLY
extern qboolean send_packet_now;
#endif

//=============================================================================

/*
============
Cmd_Wait_f

Causes execution of the remainder of the command buffer to be delayed until
next frame.  This allows commands like:
bind g "impulse 5 ; +attack ; wait ; -attack ; impulse 2"
============
*/
void Cmd_Wait_f (void)
{
	cmd_wait = true;
}


/*
=============================================================================

						COMMAND BUFFER

=============================================================================
*/

sizebuf_t	cmd_text;
byte		cmd_text_buf[65536];

byte		defer_text_buf[65536];

/*
============
Cbuf_Init
============
*/
void Cbuf_Init (void)
{
	SZ_Init (&cmd_text, cmd_text_buf, sizeof(cmd_text_buf));
}

/*
============
Cbuf_AddText

Adds command text at the end of the buffer
============
*/
void EXPORT Cbuf_AddText (char *text)
{
	int		l;
	
	l = strlen (text);

	if (cmd_text.cursize + l >= cmd_text.maxsize)
	{
		Com_Printf ("Cbuf_AddText: overflow\n");
		return;
	}
	SZ_Write (&cmd_text, text, l);
	//Com_DPrintf ("Wrote %d bytes to cmd_text: %s\n", l, text);
}


/*
============
Cbuf_InsertText

Adds command text immediately after the current command
Adds a \n to the text
FIXME: actually change the command buffer to do less copying
============
*/
void Cbuf_InsertText (char *text)
{
	char	*temp;
	int		templen;

// copy off any commands still remaining in the exec buffer
	templen = cmd_text.cursize;
	if (templen)
	{
		temp = Z_TagMalloc (templen, TAGMALLOC_CMDBUFF);
		memcpy (temp, cmd_text.data, templen);
		SZ_Clear (&cmd_text);
	}
	else
		temp = NULL;	// shut up compiler
		
// add the entire text of the file
	Cbuf_AddText (text);
	
// add the copied off data
	if (templen)
	{
		SZ_Write (&cmd_text, temp, templen);
		Z_Free (temp);
	}
}


/*
============
Cbuf_CopyToDefer
============
*/
/*void Cbuf_CopyToDefer (void)
{
	if (!cmd_text.cursize)
		return;

	memcpy(defer_text_buf, cmd_text_buf, cmd_text.cursize);
	defer_text_buf[cmd_text.cursize] = 0;

	Com_Printf ("CopyToDefer: %d bytes: %s\n", cmd_text.cursize, defer_text_buf);

	cmd_text.cursize = 0;
}*/

/*
============
Cbuf_InsertFromDefer
============
*/
/*void Cbuf_InsertFromDefer (void)
{
	Cbuf_InsertText ((char *)defer_text_buf);
	defer_text_buf[0] = 0;
}*/


/*
============
Cbuf_ExecuteText
============
*/
void EXPORT Cbuf_ExecuteText (int exec_when, char *text)
{
	switch (exec_when)
	{
	case EXEC_NOW:
		Cmd_ExecuteString (text);
		break;
	case EXEC_INSERT:
		Cbuf_InsertText (text);
		break;
	case EXEC_APPEND:
		Cbuf_AddText (text);
		break;
	default:
		Com_Error (ERR_FATAL, "Cbuf_ExecuteText: bad exec_when");
	}
}

/*
============
Cbuf_Execute
============
*/
void Cbuf_Execute (void)
{
	//qboolean	escape = false;
	int		i;
	char	*text;
	char	line[1024];
	int		quotes;

	alias_count = 0;		// don't allow infinite alias loops

	while (cmd_text.cursize)
	{
// find a \n or ; line break
		text = (char *)cmd_text.data;

		quotes = 0;
		for (i=0 ; i< cmd_text.cursize ; i++)
		{
			//r1: allow for escaped ""s for eg rcon set hostname \"House of Pain\"
			/*if (text[i] == '\\' && !escape) {
				escape = true;
				//ignore the escape character
				memcpy ((text+i), (text+i+1), cmd_text.cursize-1);
				continue;
			}*/
			
			if (text[i] == '"')
				quotes++;

			if ( !(quotes&1) && text[i] == ';')
				break;	// don't break if inside a quoted string

			//don't allow escapes of \n
			if (text[i] == '\n')
				break;

		}

		//Com_DPrintf ("Cbuf_Execute: found %d bytes\n", i);

		if (i >= sizeof(line)-1) {
			Com_Printf ("Cbuf_Execute: overflow of %d truncated\n", i);
			memcpy (line, text, sizeof(line)-1);
		} else {
			memcpy (line, text, i);
		}

		line[i] = 0;
		
// delete the text from the command buffer and move remaining commands down
// this is necessary because commands (exec, alias) can insert data at the
// beginning of the text buffer

		if (i == cmd_text.cursize)
			cmd_text.cursize = 0;
		else
		{
			i++;
			cmd_text.cursize -= i;

			if (cmd_text.cursize)
				memmove (text, text+i, cmd_text.cursize);
		}

// execute the command line
		//Com_DPrintf ("Cbuf_Execute: executing '%s'\n", line);
		//Com_Printf ("exec: %s\n", line);
		Cmd_ExecuteString (line);
		
		if (cmd_wait)
		{
			// skip out while text still remains in buffer, leaving it
			// for next frame
			//Com_Printf ("wait: breaking\n");
#ifndef DEDICATED_ONLY
			send_packet_now = true;
#endif
			cmd_wait = false;
			break;
		}
	}
}


/*
===============
Cbuf_AddEarlyCommands

Adds command line parameters as script statements
Commands lead with a +, and continue until another +

Set commands are added early, so they are guaranteed to be set before
the client and server initialize for the first time.

Other commands are added late, after all initialization is complete.
===============
*/
void Cbuf_AddEarlyCommands (qboolean clear)
{
	int		i;
	char	*s;

	for (i=0 ; i<COM_Argc() ; i++)
	{
		s = COM_Argv(i);
		if (strcmp (s, "+set"))
			continue;
		Cbuf_AddText (va("set %s %s\n", COM_Argv(i+1), COM_Argv(i+2)));
		if (clear)
		{
			COM_ClearArgv(i);
			COM_ClearArgv(i+1);
			COM_ClearArgv(i+2);
		}
		i+=2;
	}
}

/*
=================
Cbuf_AddLateCommands

Adds command line parameters as script statements
Commands lead with a + and continue until another + or -
quake +vid_ref gl +map amlev1

Returns true if any late commands were added, which
will keep the demoloop from immediately starting
=================
*/
qboolean Cbuf_AddLateCommands (void)
{
	int		i, j;
	int		s;
	char	*text, *build, c;
	int		argc;
	qboolean	ret;

	// build the combined string to parse from
	s = 0;
	argc = COM_Argc();
	for (i=1 ; i<argc ; i++)
	{
		s += strlen (COM_Argv(i)) + 1;
	}
	if (!s)
		return false;
		
	text = Z_TagMalloc (s+1, TAGMALLOC_CMDBUFF);
	text[0] = 0;
	for (i=1 ; i<argc ; i++)
	{
		strcat (text,COM_Argv(i));
		if (i != argc-1)
			strcat (text, " ");
	}
	
	// pull out the commands
	build = Z_TagMalloc (s+1, TAGMALLOC_CMDBUFF);
	build[0] = 0;
	
	for (i=0 ; i<s-1 ; i++)
	{
		if (text[i] == '+')
		{
			i++;

			for (j=i ; (text[j] != '+') && (text[j] != 0) ; j++)
				;

			c = text[j];
			text[j] = 0;
			
			strcat (build, text+i);
			strcat (build, "\n");
			text[j] = c;
			i = j-1;
		}
	}

	ret = (build[0] != 0);
	if (ret)
		Cbuf_AddText (build);
	
	Z_Free (text);
	Z_Free (build);

	return ret;
}


/*
==============================================================================

						SCRIPT COMMANDS

==============================================================================
*/


/*
===============
Cmd_Exec_f
===============
*/
void Cmd_Exec_f (void)
{
	char	*f, *f2, *p;
	int		len;

	if (Cmd_Argc () != 2)
	{
		Com_Printf ("exec <filename> : execute a script file\n");
		return;
	}

	len = FS_LoadFile (Cmd_Argv(1), (void **)&f);
	if (!f || !len)
	{
		Com_Printf ("couldn't exec %s\n",Cmd_Argv(1));
		return;
	}

	if (Com_ServerState())
		Com_Printf ("execing %s\n",Cmd_Argv(1));
	else
		Com_DPrintf ("execing %s\n",Cmd_Argv(1));
	
	// the file doesn't have a trailing 0, so we need to copy it off
	f2 = Z_TagMalloc(len+2, TAGMALLOC_CMDBUFF);
	memcpy (f2, f, len);

	//r1: fix for "no trailing newline = 'u or s'" bug.
	f2[len] = '\n';
	f2[len+1] = 0;

	if ((p = strstr(f2, "\r")) && *(p+1) != '\n')
		Com_Printf ("WARNING: Raw \\r found in config file %s\n", Cmd_Argv(1));

	Cbuf_InsertText (f2);

	Z_Free (f2);
	FS_FreeFile (f);
}


/*
===============
Cmd_Echo_f

Just prints the rest of the line to the console
===============
*/
void Cmd_Echo_f (void)
{
	int		i;
	
	for (i=1 ; i<Cmd_Argc() ; i++)
		Com_Printf ("%s ",Cmd_Argv(i));
	Com_Printf ("\n");
}

static int EXPORT aliassort( const void *_a, const void *_b )
{
	const cmdalias_t	*a = (const cmdalias_t *)_a;
	const cmdalias_t	*b = (const cmdalias_t *)_b;

	return strcmp (a->name, b->name);
}

void Cmd_Aliaslist_f (void)
{
	cmdalias_t	*a;
	int argLen;
	int i, j;
	int len, num;
	cmdalias_t *sortedList;

	argLen = strlen(Cmd_Argv(1));

	for (a = cmd_alias, i = 0; a ; a = a->next, i++);
	num = i;

	len = num * sizeof(cmdalias_t);
	sortedList = Z_TagMalloc (len, TAGMALLOC_CMD);
	
	for (a = cmd_alias, i = 0; a ; a = a->next, i++)
	{
		sortedList[i] = *a;
	}

	qsort (sortedList, num, sizeof(sortedList[0]), (int (EXPORT *)(const void *, const void *))aliassort);

	//for (a = cmd_alias ; a ; a=a->next)
	for (j = 0; j < num; j++)
	{
		a = &sortedList[j];

		if (argLen && Q_strncasecmp (a->name, Cmd_Argv(1), argLen))
			continue;

		if (argLen)
			Com_Printf ("a %s\n", a->name);
		else
			Com_Printf ("%s : %s\n", a->name, a->value);
	}

	Z_Free (sortedList);
}

/*
===============
Cmd_Alias_f

Creates a new command that executes a command string (possibly ; seperated)
===============
*/
void Cmd_Alias_f (void)
{
	cmdalias_t	*a;
	char		cmd[1024];
	int			i, c;
	char		*s;

	if (Cmd_Argc() == 1)
	{
		Com_Printf ("Current alias commands:\n");
		Cmd_Aliaslist_f ();
		return;
	}

	s = Cmd_Argv(1);
	if (strlen(s) >= MAX_ALIAS_NAME)
	{
		Com_Printf ("Alias name is too long\n");
		return;
	}

	// if the alias already exists, reuse it
	for (a = cmd_alias ; a ; a=a->next)
	{
		if (!strcmp(s, a->name))
		{
			Z_Free (a->value);
			break;
		}
	}

	if (!a)
	{
		a = Z_TagMalloc (sizeof(cmdalias_t), TAGMALLOC_CMDBUFF);
		a->next = cmd_alias;
		cmd_alias = a;
	}
	strcpy (a->name, s);	

// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	c = Cmd_Argc();
	for (i=2 ; i< c ; i++)
	{
		strcat (cmd, Cmd_Argv(i));
		if (i != (c - 1))
			strcat (cmd, " ");
	}
	strcat (cmd, "\n");
	
	a->value = CopyString (cmd);
}

/*
=============================================================================

					COMMAND EXECUTION

=============================================================================
*/

static	int			cmd_argc;
static	char		*cmd_argv[MAX_STRING_TOKENS];
static	char		*cmd_null_string = "";
static	char		cmd_args[MAX_STRING_CHARS];

cmd_function_t	*cmd_functions;		// possible commands to execute

/*
============
Cmd_Argc
============
*/
int		EXPORT Cmd_Argc (void)
{
	return cmd_argc;
}

/*
============
Cmd_Argv
============
*/
char	* EXPORT Cmd_Argv (int arg)
{
	if ( (unsigned)arg >= cmd_argc )
		return cmd_null_string;
	return cmd_argv[arg];	
}

/*
============
Cmd_Args

Returns a single string containing argv(1) to argv(argc()-1)
============
*/
char		* EXPORT Cmd_Args (void)
{
	return cmd_args;
}

//r1ch: added this, returns all args from specified argv offset until argc()-1
char *Cmd_Args2 (int arg)
{
	static char args[1024];
	int i;

	args[0] = 0;

	for (i = arg; i < cmd_argc; i++)
	{
		if (strlen(args) + (strlen(cmd_argv[i])+2) >= sizeof(args))
		{
			Com_Printf ("Cmd_Args2: overflow\n");
			break;
		}
		strcat (args, cmd_argv[i]);
		if (i != (cmd_argc-1))
			strcat (args, " ");
	}

	return args;
}


/*
======================
Cmd_MacroExpandString
======================
*/
char *Cmd_MacroExpandString (char *text)
{
	int		i, j, count, len;
	qboolean	inquote;
	char	*scan;
	static	char	expanded[MAX_STRING_CHARS];
	char	temporary[MAX_STRING_CHARS];
	char	*token, *start;

	if (!strstr(text, "$"))
		return text;

	inquote = false;
	scan = text;

	len = strlen (scan);
	if (len >= MAX_STRING_CHARS)
	{
		Com_Printf ("Line exceeded %i chars, discarded.\n", MAX_STRING_CHARS);
		return NULL;
	}

	count = 0;

	for (i=0 ; i<len ; i++)
	{
		if (scan[i] == '"')
			inquote ^= 1;
		if (inquote)
			continue;	// don't expand inside quotes
		if (scan[i] != '$')
			continue;
		// scan out the complete macro
		start = scan+i+1;
		token = COM_Parse (&start);
		if (!start)
			continue;
	
		token = Cvar_VariableString (token);

		j = strlen(token);
		len += j;
		if (len >= MAX_STRING_CHARS)
		{
			Com_Printf ("Expanded line exceeded %i chars, discarded.\n", MAX_STRING_CHARS);
			return NULL;
		}

		strncpy (temporary, scan, i);
		strcpy (temporary+i, token);
		strcpy (temporary+i+j, start);

		strcpy (expanded, temporary);
		scan = expanded;
		i--;

		if (++count == 100)
		{
			Com_Printf ("Macro expansion loop, discarded.\n");
			return NULL;
		}
	}

	if (inquote)
	{
		Com_Printf ("Line '%s' has unmatched quote, discarded.\n", text);
		return NULL;
	}

	return scan;
}


/*
============
Cmd_TokenizeString

Parses the given string into command line tokens.
$Cvars will be expanded unless they are in a quoted token
============
*/
void Cmd_TokenizeString (char *text, qboolean macroExpand)
{
	int		i;
	char	*com_token;

// clear the args from the last string
	for (i=0 ; i<cmd_argc ; i++)
		Z_Free (cmd_argv[i]);
		
	cmd_argc = 0;
	cmd_args[0] = 0;
	
	// macro expand the text
	if (macroExpand)
		text = Cmd_MacroExpandString (text);

	if (!text)
		return;

	for (;;)
	{
// skip whitespace up to a /n
		while (*text && *text <= ' ' && *text != '\n')
		{
			text++;
		}
		
		if (*text == '\n')
		{	// a newline seperates commands in the buffer
			text++;
			break;
		}

		if (!*text)
			return;

		// set cmd_args to everything after the first arg
		if (cmd_argc == 1)
		{
			int		l;

			strncpy (cmd_args, text, sizeof(cmd_args)-1);

			// strip off any trailing whitespace
			l = strlen(cmd_args) - 1;
			if (l == 1022) {
				Com_Printf ("Cmd_TokenizeString: overflowed, possible attack detected.\nargv[0] = %s, remote = %s, len = %d\n", cmd_argv[0], NET_AdrToString(net_from), net_message.cursize);
				return;
			}
			for ( ; l >= 0 ; l--)
				if (cmd_args[l] <= ' ')
					cmd_args[l] = 0;
				else
					break;
		}
			
		com_token = COM_Parse (&text);
		if (!text)
			return;

		if (cmd_argc < MAX_STRING_TOKENS)
		{
			cmd_argv[cmd_argc] = Z_TagMalloc (strlen(com_token)+1, TAGMALLOC_CMDTOKEN);
			strcpy (cmd_argv[cmd_argc], com_token);
			cmd_argc++;
		}
	}
	
}


/*
============
Cmd_AddCommand
============
*/
void	EXPORT Cmd_AddCommand (char *cmd_name, xcommand_t function)
{
	cmd_function_t	*cmd;
	
// fail if the command is a variable name
	if (Cvar_VariableString(cmd_name)[0])
	{
		Com_Printf ("Cmd_AddCommand: %s already defined as a var\n", cmd_name);
		return;
	}
	
// fail if the command already exists
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!strcmp (cmd_name, cmd->name))
		{
			Com_Printf ("Cmd_AddCommand: %s already defined\n", cmd_name);
			return;
		}
	}

	cmd = Z_TagMalloc (sizeof(cmd_function_t), TAGMALLOC_CMD);
	cmd->name = cmd_name;
	cmd->function = function;
	cmd->next = cmd_functions;
	cmd_functions = cmd;
}

/*
============
Cmd_RemoveCommand
============
*/
void	EXPORT Cmd_RemoveCommand (char *cmd_name)
{
	cmd_function_t	*cmd, **back;

	back = &cmd_functions;
	for (;;)
	{
		cmd = *back;
		if (!cmd)
		{
			Com_Printf ("Cmd_RemoveCommand: %s not added\n", cmd_name);
			return;
		}
		if (!strcmp (cmd_name, cmd->name))
		{
			*back = cmd->next;
			Z_Free (cmd);
			return;
		}
		back = &cmd->next;
	}
}

/*
============
Cmd_Exists
============
*/
qboolean	Cmd_Exists (char *cmd_name)
{
	cmd_function_t	*cmd;

	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!strcmp (cmd_name,cmd->name))
			return true;
	}

	return false;
}



/*
============
Cmd_CompleteCommand
============
*/
/*char *Cmd_CompleteCommand (char *partial)
{
	cmd_function_t	*cmd;
	int				len;
	cmdalias_t		*a;
	
	len = strlen(partial);
	
	if (!len)
		return NULL;
		
// check for exact match
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (!strcmp (partial,cmd->name))
			return cmd->name;
	for (a=cmd_alias ; a ; a=a->next)
		if (!strcmp (partial, a->name))
			return a->name;

// check for partial match
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
		if (!strncmp (partial,cmd->name, len)) {
			return cmd->name;
		}
	for (a=cmd_alias ; a ; a=a->next)
		if (!strncmp (partial, a->name, len))
			return a->name;

	return NULL;
}*/

// JPG 1.05 - completely rewrote this; includes aliases
char *Cmd_CompleteCommand (char *partial)
{
	cmd_function_t	*cmd;
	char *best = "~";
	char *least = "~";
	cmdalias_t *alias;

	for (cmd = cmd_functions ; cmd ; cmd = cmd->next)
	{
		if (strcmp(cmd->name, partial) >= 0 && strcmp(best, cmd->name) > 0)
			best = cmd->name;
		if (strcmp(cmd->name, least) < 0)
			least = cmd->name;
	}
	for (alias = cmd_alias ; alias ; alias = alias->next)
	{
		if (strcmp(alias->name, partial) >= 0 && strcmp(best, alias->name) > 0)
			best = alias->name;
		if (strcmp(alias->name, least) < 0)
			least = alias->name;
	}
	if (best[0] == '~')
		return least;
	return best;
}


/*
============
Cmd_ExecuteString

A complete command line has been parsed, so try to execute it
FIXME: lookupnoadd the token to speed search?
============
*/
void	Cmd_ExecuteString (char *text)
{	
	cmd_function_t	*cmd;
	cmdalias_t		*a;

	Cmd_TokenizeString (text, true);
			
	// execute the command line
	if (!Cmd_Argc()) {
		Com_DPrintf ("Cmd_ExecuteString: no tokens on '%s'\n", text);
		return;		// no tokens
	}

	// check functions
	for (cmd=cmd_functions ; cmd ; cmd=cmd->next)
	{
		if (!Q_strcasecmp (cmd_argv[0],cmd->name))
		{
			if (!cmd->function)
			{	// forward to server command
				Cmd_ExecuteString (va("cmd %s", text));
				Com_DPrintf ("Cmd_ExecuteString: no function '%s' for '%s', using 'cmd'\n", cmd->name, text);
			}
			else
			{
				//Com_DPrintf ("Cmd_ExecuteString: function '%s' called for '%s'\n", cmd->name, text);
				cmd->function ();
			}
			return;
		}
	}

	// check alias
	for (a=cmd_alias ; a ; a=a->next)
	{
		if (!Q_strcasecmp (cmd_argv[0], a->name))
		{
			if (++alias_count == ALIAS_LOOP_COUNT)
			{
				Com_Printf ("ALIAS_LOOP_COUNT\n");
				return;
			}
			Cbuf_InsertText (a->value);
			return;
		}
	}
	
	// check cvars
	if (Cvar_Command ()) {
		Com_DPrintf ("Cmd_ExecuteString: '%s' : is cvar\n", text);
		return;
	}

	// send it as a server command if we are connected
#ifndef DEDICATED_ONLY
	Cmd_ForwardToServer ();
#else
	Com_Printf ("Unknown command \"%s\"\n", text);
#endif
}

/*
============
Cmd_List_f
============
*/
static int EXPORT cmdsort( const void *_a, const void *_b )
{
	const cmd_function_t	*a = (const cmd_function_t *)_a;
	const cmd_function_t	*b = (const cmd_function_t *)_b;

	return strcmp (a->name, b->name);
}

void Cmd_List_f (void)
{
	cmd_function_t	*cmd;
	int				i, j;
	int				len, num;
	int				argLen;
	cmd_function_t	*sortedList;

	argLen = strlen(Cmd_Argv(1));

	for (cmd = cmd_functions, i = 0; cmd ; cmd = cmd->next, i++);
	num = i;

	len = num * sizeof(cmd_function_t);
	sortedList = Z_TagMalloc (len, TAGMALLOC_CMD);
	
	for (cmd = cmd_functions, i = 0; cmd ; cmd = cmd->next, i++)
	{
		sortedList[i] = *cmd;
	}

	qsort (sortedList, num, sizeof(sortedList[0]), (int (EXPORT *)(const void *, const void *))cmdsort);

	//for (cmd=cmd_functions ; cmd ; cmd=cmd->next, i++)
	for (j = 0; j < num; j++)
	{
		cmd = &sortedList[j];

		if (argLen && Q_strncasecmp (cmd->name, Cmd_Argv(1), argLen))
			continue;
		Com_Printf ("c %s\n", cmd->name);
	}

	if (!argLen)
		Com_Printf ("%i commands\n", i);

	Z_Free (sortedList);
}

/*
============
Cmd_Init
============
*/
void Cmd_Init (void)
{
//
// register our commands
//
	Cmd_AddCommand ("cmdlist",Cmd_List_f);
	Cmd_AddCommand ("exec",Cmd_Exec_f);
	Cmd_AddCommand ("echo",Cmd_Echo_f);
	Cmd_AddCommand ("alias",Cmd_Alias_f);
	Cmd_AddCommand ("aliaslist",Cmd_Aliaslist_f);
	Cmd_AddCommand ("wait", Cmd_Wait_f);
}

