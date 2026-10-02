/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).  

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================

Adapted from DOOM-3 neo/game/script/Script_Interpreter.cpp. Changed: entity and game sys events are handed to the game module as call records and the thread waits for the result; engine events dispatch through idThread::ProcessEngineEvent with intptr_t argument slots; errors kill the thread instead of the game; the runaway limit is per thread per frame and kills the thread; new threads start after the current one stops.

===========================================================================
*/

#include "Script_Local.h"

/*
================
idInterpreter::idInterpreter()
================
*/
idInterpreter::idInterpreter() {
	localstackUsed = 0;
	terminateOnExit = true;
	debug = 0;
	memset( localstack, 0, sizeof( localstack ) );
	memset( callStack, 0, sizeof( callStack ) );
	Reset();
}

/*
================
idInterpreter::Reset
================
*/
void idInterpreter::Reset( void ) {
	callStackDepth = 0;
	localstackUsed = 0;
	localstackBase = 0;

	maxLocalstackUsed = 0;
	maxStackDepth = 0;

	popParms = 0;
	eventEntity = ENTITY_NONE;
	awaitingGame = false;

	currentFunction = 0;
	NextInstruction( 0 );

	threadDying 	= false;
	doneProcessing	= true;
}

/*
================
idInterpreter::GetRegisterValue

Returns a string representation of the value of the register.  This is 
used primarily for the debugger and debugging

//FIXME:  This is pretty much wrong.  won't access data in most situations.
================
*/
bool idInterpreter::GetRegisterValue( const char *name, idStr &out, int scopeDepth ) {
	varEval_t		reg;
	idVarDef		*d;
	char			funcObject[ 1024 ];
	char			*funcName;
	const idVarDef	*scope;
	const idTypeDef	*field;
	const idScriptObject *obj;
	const function_t *func;

	out.Empty();
	
	if ( scopeDepth == -1 ) {
		scopeDepth = callStackDepth;
	}	
	
	if ( scopeDepth == callStackDepth ) {
		func = currentFunction;
	} else {
		func = callStack[ scopeDepth ].f;
	}
	if ( !func ) {
		return false;
	}

	idStr::Copynz( funcObject, func->Name(), sizeof( funcObject ) );
	funcName = strstr( funcObject, "::" );
	if ( funcName ) {
		*funcName = '\0';
		scope = gameLocal.program.GetDef( NULL, funcObject, &def_namespace );
		funcName += 2;
	} else {
		funcName = funcObject;
		scope = &def_namespace;
	}

	// Get the function from the object
	d = gameLocal.program.GetDef( NULL, funcName, scope );
	if ( !d ) {
		return false;
	}
	
	// Get the variable itself and check various namespaces
	d = gameLocal.program.GetDef( NULL, name, d );
	if ( !d ) {
		if ( scope == &def_namespace ) {
			return false;
		}
		
		d = gameLocal.program.GetDef( NULL, name, scope );
		if ( !d ) {
			d = gameLocal.program.GetDef( NULL, name, &def_namespace );
			if ( !d ) {
				return false;
			}
		}
	}
		
	reg = GetVariable( d );
	switch( d->Type() ) {
	case ev_float:
		if ( reg.floatPtr ) {
			out = va("%g", *reg.floatPtr );
		} else {
			out = "0";
		}
		return true;
		break;

	case ev_vector:
		if ( reg.vectorPtr ) {				
			out = va( "%g,%g,%g", reg.vectorPtr->x, reg.vectorPtr->y, reg.vectorPtr->z );
		} else {
			out = "0,0,0";
		}
		return true;
		break;

	case ev_boolean:
		if ( reg.intPtr ) {
			out = va( "%d", *reg.intPtr );
		} else {
			out = "0";
		}
		return true;
		break;

	case ev_field:
		if ( scope == &def_namespace ) {
			// should never happen, but handle it safely anyway
			return false;
		}

		field = scope->TypeDef()->GetParmType( reg.ptrOffset )->FieldType();
		obj   = *reinterpret_cast<const idScriptObject **>( &localstack[ callStack[ callStackDepth ].stackbase ] );
		if ( !field || !obj ) {
			return false;
		}
								
		switch ( field->Type() ) {
		case ev_boolean:
			out = va( "%d", *( reinterpret_cast<int *>( &obj->data[ reg.ptrOffset ] ) ) );
			return true;

		case ev_float:
			out = va( "%g", *( reinterpret_cast<float *>( &obj->data[ reg.ptrOffset ] ) ) );
			return true;

		default:
			return false;
		}
		break;

	case ev_string:
		if ( reg.stringPtr ) {
			out = "\"";
			out += reg.stringPtr;
			out += "\"";
		} else {
			out = "\"\"";
		}
		return true;

	default:
		return false;
	}
}

/*
================
idInterpreter::GetCallstackDepth
================
*/
int idInterpreter::GetCallstackDepth( void ) const {
	return callStackDepth;
}

/*
================
idInterpreter::GetCallstack
================
*/
const prstack_t *idInterpreter::GetCallstack( void ) const {
	return &callStack[ 0 ];
}

/*
================
idInterpreter::GetCurrentFunction
================
*/
const function_t *idInterpreter::GetCurrentFunction( void ) const {
	return currentFunction;
}

/*
================
idInterpreter::GetThread
================
*/
idThread *idInterpreter::GetThread( void ) const {
	return thread;
}


/*
================
idInterpreter::SetThread
================
*/
void idInterpreter::SetThread( idThread *pThread ) {
	thread = pThread;
}

/*
================
idInterpreter::CurrentLine
================
*/
int idInterpreter::CurrentLine( void ) const {
	if ( instructionPointer < 0 ) {
		return 0;
	}
	return gameLocal.program.GetLineNumberForStatement( instructionPointer );
}

/*
================
idInterpreter::CurrentFile
================
*/
const char *idInterpreter::CurrentFile( void ) const {
	if ( instructionPointer < 0 ) {
		return "";
	}
	return gameLocal.program.GetFilenameForStatement( instructionPointer );
}

/*
============
idInterpreter::StackTrace
============
*/
void idInterpreter::StackTrace( void ) const {
	const function_t	*f;
	int 				i;
	int					top;

	if ( callStackDepth == 0 ) {
		gameLocal.Printf( "<NO STACK>\n" );
		return;
	}

	top = callStackDepth;
	if ( top >= MAX_STACK_DEPTH ) {
		top = MAX_STACK_DEPTH - 1;
	}
	
	if ( !currentFunction ) {
		gameLocal.Printf( "<NO FUNCTION>\n" );
	} else {
		gameLocal.Printf( "%12s : %s\n", gameLocal.program.GetFilename( currentFunction->filenum ), currentFunction->Name() );
	}

	for( i = top; i >= 0; i-- ) {
		f = callStack[ i ].f;
		if ( !f ) {
			gameLocal.Printf( "<NO FUNCTION>\n" );
		} else {
			gameLocal.Printf( "%12s : %s\n", gameLocal.program.GetFilename( f->filenum ), f->Name() );
		}
	}
}

/*
============
idInterpreter::Error

Aborts the currently executing function

oax: D3 stopped the game (common->Error). Here the thread is killed: the
message goes to the console and the jump set by idThread::Execute.
============
*/
void idInterpreter::Error( const char *fmt, ... ) const {
	va_list argptr;
	char	text[ 1024 ];
	char	msg[ 1024 ];

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	StackTrace();

	if ( ( instructionPointer >= 0 ) && ( instructionPointer < gameLocal.program.NumStatements() ) ) {
		statement_t &line = gameLocal.program.GetStatement( instructionPointer );
		idStr::snPrintf( msg, sizeof( msg ), "%s(%d): Thread '%s': %s", gameLocal.program.GetFilename( line.file ), line.linenumber, thread->GetThreadName(), text );
	} else {
		idStr::snPrintf( msg, sizeof( msg ), "Thread '%s': %s", thread->GetThreadName(), text );
	}
	Script_ThreadError( msg );
}

/*
============
idInterpreter::Warning

Prints file and line number information with warning.
============
*/
void idInterpreter::Warning( const char *fmt, ... ) const {
	va_list argptr;
	char	text[ 1024 ];

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	if ( ( instructionPointer >= 0 ) && ( instructionPointer < gameLocal.program.NumStatements() ) ) {
		statement_t &line = gameLocal.program.GetStatement( instructionPointer );
		gameLocal.Warning( "%s(%d): Thread '%s': %s", gameLocal.program.GetFilename( line.file ), line.linenumber, thread->GetThreadName(), text );
	} else {
		gameLocal.Warning( "Thread '%s' : %s", thread->GetThreadName(), text );
	}
}

/*
================
idInterpreter::DisplayInfo
================
*/
void idInterpreter::DisplayInfo( void ) const {
	const function_t *f;
	int i;

	gameLocal.Printf( " Stack depth: %d bytes, %d max\n", localstackUsed, maxLocalstackUsed );
	gameLocal.Printf( "  Call depth: %d, %d max\n", callStackDepth, maxStackDepth );
	gameLocal.Printf( "  Call Stack: " );

	if ( callStackDepth == 0 ) {
		gameLocal.Printf( "<NO STACK>\n" );
	} else {
		if ( !currentFunction ) {
			gameLocal.Printf( "<NO FUNCTION>\n" );
		} else {
			gameLocal.Printf( "%12s : %s\n", gameLocal.program.GetFilename( currentFunction->filenum ), currentFunction->Name() );
		}

		for( i = callStackDepth; i > 0; i-- ) {
			gameLocal.Printf( "              " );
			f = callStack[ i ].f;
			if ( !f ) {
				gameLocal.Printf( "<NO FUNCTION>\n" );
			} else {
				gameLocal.Printf( "%12s : %s\n", gameLocal.program.GetFilename( f->filenum ), f->Name() );
			}
		}
	}
}

/*
====================
idInterpreter::ThreadCall

Copys the args from the calling thread's stack
====================
*/
void idInterpreter::ThreadCall( idInterpreter *source, const function_t *func, int args ) {
	Reset();

	memcpy( localstack, &source->localstack[ source->localstackUsed - args ], args );

	localstackUsed = args;
	localstackBase = 0;

	maxLocalstackUsed = localstackUsed;
	EnterFunction( func, false );

	thread->SetThreadName( currentFunction->Name() );
}

/*
================
idInterpreter::EnterObjectFunction

Calls a function on a script object.

NOTE: If this is called from within a event called by this interpreter, the function arguments will be invalid after calling this function.
================
*/
void idInterpreter::EnterObjectFunction( scriptEntity_t self, const function_t *func, bool clearStack ) {
	if ( clearStack ) {
		Reset();
	}
	if ( popParms ) {
		PopParms( popParms );
		popParms = 0;
	}
	Push( self );
	EnterFunction( func, false );
}

/*
====================
idInterpreter::EnterFunction

Returns the new program statement counter

NOTE: If this is called from within a event called by this interpreter, the function arguments will be invalid after calling this function.
====================
*/
void idInterpreter::EnterFunction( const function_t *func, bool clearStack ) {
	int 		c;
	prstack_t	*stack;

	if ( clearStack ) {
		Reset();
	}
	if ( popParms ) {
		PopParms( popParms );
		popParms = 0;
	}

	if ( callStackDepth >= MAX_STACK_DEPTH ) {
		Error( "call stack overflow" );
	}

	stack = &callStack[ callStackDepth ];

	stack->s			= instructionPointer + 1;	// point to the next instruction to execute
	stack->f			= currentFunction;
	stack->stackbase	= localstackBase;

	callStackDepth++;
	if ( callStackDepth > maxStackDepth ) {
		maxStackDepth = callStackDepth;
	}

	if ( !func ) {
		Error( "NULL function" );
	}

	if ( debug ) {
		if ( currentFunction ) {
			gameLocal.Printf( "%d: call '%s' from '%s'(line %d)%s\n", gameLocal.time, func->Name(), currentFunction->Name(), 
				gameLocal.program.GetStatement( instructionPointer ).linenumber, clearStack ? " clear stack" : "" );
		} else {
            gameLocal.Printf( "%d: call '%s'%s\n", gameLocal.time, func->Name(), clearStack ? " clear stack" : "" );
		}
	}

	currentFunction = func;
	assert( !func->eventdef );
	NextInstruction( func->firstStatement );

	// allocate space on the stack for locals
	// parms are already on stack
	c = func->locals - func->parmTotal;
	assert( c >= 0 );

	if ( localstackUsed + c > LOCALSTACK_SIZE ) {
		Error( "EnterFuncton: locals stack overflow\n" );
	}

	// initialize local stack variables to zero
	memset( &localstack[ localstackUsed ], 0, c );

	localstackUsed += c;
	localstackBase = localstackUsed - func->locals;

	if ( localstackUsed > maxLocalstackUsed ) {
		maxLocalstackUsed = localstackUsed ;
	}
}

/*
====================
idInterpreter::LeaveFunction
====================
*/
void idInterpreter::LeaveFunction( idVarDef *returnDef ) {
	prstack_t *stack;
	varEval_t ret;
	
	if ( callStackDepth <= 0 ) {
		Error( "prog stack underflow" );
	}

	// return value
	if ( returnDef ) {
		switch( returnDef->Type() ) {
		case ev_string :
			gameLocal.program.ReturnString( GetString( returnDef ) );
			break;

		case ev_vector :
			ret = GetVariable( returnDef );
			gameLocal.program.ReturnVector( *ret.vectorPtr );
			break;

		default :
			ret = GetVariable( returnDef );
			gameLocal.program.ReturnInteger( *ret.intPtr );
		}
	}

	// remove locals from the stack
	PopParms( currentFunction->locals );
	assert( localstackUsed == localstackBase );

	if ( debug ) {
		statement_t &line = gameLocal.program.GetStatement( instructionPointer );
		gameLocal.Printf( "%d: %s(%d): exit %s", gameLocal.time, gameLocal.program.GetFilename( line.file ), line.linenumber, currentFunction->Name() );
		if ( callStackDepth > 1 ) {
			gameLocal.Printf( " return to %s(line %d)\n", callStack[ callStackDepth - 1 ].f->Name(), gameLocal.program.GetStatement( callStack[ callStackDepth - 1 ].s ).linenumber );
		} else {
			gameLocal.Printf( " done\n" );
		}
	}

	// up stack
	callStackDepth--;
	stack = &callStack[ callStackDepth ]; 
	currentFunction = stack->f;
	localstackBase = stack->stackbase;
	NextInstruction( stack->s );

	if ( !callStackDepth ) {
		// all done
		doneProcessing = true;
		threadDying = true;
		currentFunction = 0;
	}
}

/*
================
idInterpreter::MarshalEventArgs

oax: copies the arguments of an event call from the local stack into a
call record for the game module (D3 built an int data[] array for
idClass::ProcessEventArgPtr). Returns false, with the thread ending, when
an 'e' argument is null (D3: "Entity not found for event").
================
*/
bool idInterpreter::MarshalEventArgs( const function_t *func, const idEventDef *evdef, int start, int pos, int argsize, oaxScriptCall_t *call ) {
	int			i, j, len, strUsed;
	varEval_t	var;
	const char	*format;
	const char	*str;

	format = evdef->GetArgFormat();
	strUsed = 0;
	call->argc = 0;
	for( j = 0, i = 0; ( pos < argsize ) || ( format[ i ] != 0 ); i++ ) {
		oaxScriptValue_t *v = &call->args[ i ];

		memset( v, 0, sizeof( *v ) );
		var.intPtr = ( int * )&localstack[ start + pos ];
		switch( format[ i ] ) {
		case D_EVENT_INTEGER :
			v->type = OAX_SV_INT;
			v->i = int( *var.floatPtr );
			break;

		case D_EVENT_FLOAT :
			v->type = OAX_SV_FLOAT;
			v->f[ 0 ] = *var.floatPtr;
			break;

		case D_EVENT_VECTOR :
			v->type = OAX_SV_VECTOR;
			v->f[ 0 ] = var.vectorPtr->x;
			v->f[ 1 ] = var.vectorPtr->y;
			v->f[ 2 ] = var.vectorPtr->z;
			break;

		case D_EVENT_STRING :
			str = ( char * )&localstack[ start + pos ];
			len = idStr::Length( str );
			if ( strUsed + len + 1 > OAX_SCRIPT_STRINGS ) {
				len = OAX_SCRIPT_STRINGS - strUsed - 1;
			}
			v->type = OAX_SV_STRING;
			v->i = strUsed;
			memcpy( call->strings + strUsed, str, len );
			call->strings[ strUsed + len ] = '\0';
			strUsed += len + 1;
			break;

		case D_EVENT_ENTITY :
			v->type = OAX_SV_ENTITY;
			v->i = *var.entityNumberPtr;
			if ( v->i == ENTITY_NONE ) {
				Warning( "Entity not found for event '%s'. Terminating thread.", evdef->GetName() );
				threadDying = true;
				PopParms( argsize );
				return false;
			}
			break;

		case D_EVENT_ENTITY_NULL :
			v->type = OAX_SV_ENTITY;
			v->i = *var.entityNumberPtr;
			break;

		case D_EVENT_TRACE :
			Error( "trace type not supported from script for '%s' event.", evdef->GetName() );
			break;

		default :
			Error( "Invalid arg format string for '%s' event.", evdef->GetName() );
			break;
		}

		call->argc = i + 1;
		pos += func->parmSize[ j++ ];
	}
	return true;
}

/*
================
idInterpreter::BeginGameCall

oax: the thread stops here until the game module returns the result
(OAXScript_Return); the arguments stay on the stack until then.
================
*/
void idInterpreter::BeginGameCall( int argsize ) {
	popParms = argsize;
	awaitingGame = true;
	doneProcessing = true;
}

/*
================
idInterpreter::EndGameCall

oax: the game module returned; pop the event's arguments and go on.
================
*/
void idInterpreter::EndGameCall( void ) {
	if ( popParms ) {
		PopParms( popParms );
	}
	popParms = 0;
	awaitingGame = false;
	eventEntity = ENTITY_NONE;
}

/*
================
idInterpreter::CallEvent

oax: entity events are executed by the game module. The call is handed to
it through the script pump and the thread waits for the result.
================
*/
void idInterpreter::CallEvent( const function_t *func, int argsize ) {
	int 				start;
	const idEventDef	*evdef;
	oaxScriptCall_t		*call;

	if ( !func ) {
		Error( "NULL function" );
	}

	assert( func->eventdef );
	evdef = func->eventdef;

	start = localstackUsed - argsize;
	eventEntity = *( int * )&localstack[ start ];

	if ( eventEntity == ENTITY_NONE ) {
		// always return a safe value when an object doesn't exist
		switch( evdef->GetReturnType() ) {
		case D_EVENT_INTEGER :
			gameLocal.program.ReturnInteger( 0 );
			break;

		case D_EVENT_FLOAT :
			gameLocal.program.ReturnFloat( 0 );
			break;

		case D_EVENT_VECTOR :
			gameLocal.program.ReturnVector( vec3_zero );
			break;

		case D_EVENT_STRING :
			gameLocal.program.ReturnString( "" );
			break;

		case D_EVENT_ENTITY :
		case D_EVENT_ENTITY_NULL :
			gameLocal.program.ReturnEntity( ENTITY_NONE );
			break;

		case D_EVENT_TRACE :
		default:
			// unsupported data type
			break;
		}

		PopParms( argsize );
		eventEntity = ENTITY_NONE;
		return;
	}

	call = idThread::GameCallRecord();
	if ( !MarshalEventArgs( func, evdef, start, type_object.Size(), argsize, call ) ) {
		return;
	}
	call->self = eventEntity;
	thread->PostGameCall( evdef, eventEntity, GAMECALL_EVENT );
	BeginGameCall( argsize );
}

/*
================
idInterpreter::CallSysEvent

oax: thread events the engine implements run right here (D3:
thread->ProcessEventArgPtr). The game module's sys events go through the
pump like entity events.
================
*/
void idInterpreter::CallSysEvent( const function_t *func, int argsize ) {
	int 				i;
	int					j;
	varEval_t			source;
	int 				pos;
	int 				start;
	intptr_t			data[ D_EVENT_MAXARGS ];
	idVec3				vectors[ D_EVENT_MAXARGS ];
	const idEventDef	*evdef;
	const char			*format;

	if ( !func ) {
		Error( "NULL function" );
	}

	assert( func->eventdef );
	evdef = func->eventdef;

	start = localstackUsed - argsize;

	if ( !evdef->IsEngineEvent() ) {
		oaxScriptCall_t *call = idThread::GameCallRecord();
		if ( !MarshalEventArgs( func, evdef, start, 0, argsize, call ) ) {
			return;
		}
		call->self = ENTITY_NONE;
		thread->PostGameCall( evdef, ENTITY_NONE, GAMECALL_EVENT );
		BeginGameCall( argsize );
		return;
	}

	format = evdef->GetArgFormat();
	for( j = 0, i = 0, pos = 0; ( pos < argsize ) || ( format[ i ] != 0 ); i++ ) {
		switch( format[ i ] ) {
		case D_EVENT_INTEGER :
			source.intPtr = ( int * )&localstack[ start + pos ];
			data[ i ] = int( *source.floatPtr );
			break;

		case D_EVENT_FLOAT :
			source.intPtr = ( int * )&localstack[ start + pos ];
			*( float * )&data[ i ] = *source.floatPtr;
			break;

		case D_EVENT_VECTOR :
			source.intPtr = ( int * )&localstack[ start + pos ];
			vectors[ i ] = *source.vectorPtr;
			data[ i ] = ( intptr_t )&vectors[ i ];
			break;

		case D_EVENT_STRING :
			data[ i ] = ( intptr_t )( char * )&localstack[ start + pos ];
			break;

		case D_EVENT_ENTITY :
			source.intPtr = ( int * )&localstack[ start + pos ];
			data[ i ] = *source.entityNumberPtr;
			if ( !data[ i ] ) {
				Warning( "Entity not found for event '%s'. Terminating thread.", evdef->GetName() );
				threadDying = true;
				PopParms( argsize );
				return;
			}
			break;

		case D_EVENT_ENTITY_NULL :
			source.intPtr = ( int * )&localstack[ start + pos ];
			data[ i ] = *source.entityNumberPtr;
			break;

		case D_EVENT_TRACE :
			Error( "trace type not supported from script for '%s' event.", evdef->GetName() );
			break;

		default :
			Error( "Invalid arg format string for '%s' event.", evdef->GetName() );
			break;
		}

		pos += func->parmSize[ j++ ];
	}

	popParms = argsize;
	thread->ProcessEngineEvent( evdef, data );
	if ( awaitingGame ) {
		// the event handed a call to the game module (sys.waitFor); the
		// arguments are popped when it returns
		return;
	}
	if ( popParms ) {
		PopParms( popParms );
	}
	popParms = 0;
}

/*
====================
idInterpreter::Execute
====================
*/
bool idInterpreter::Execute( void ) {
	varEval_t	var_a;
	varEval_t	var_b;
	varEval_t	var_c;
	varEval_t	var;
	statement_t	*st;
	idThread	*newThread;
	float		floatVal;
	idScriptObject *obj;
	const function_t *func;

	if ( threadDying || !currentFunction ) {
		return true;
	}

	if ( awaitingGame ) {
		// oax: still waiting for the game module's result
		return false;
	}

	doneProcessing = false;
	while( !doneProcessing && !threadDying ) {
		instructionPointer++;

		// oax: D3 allowed 5000000 instructions per Execute and then stopped
		// the game. Here each thread may run script_maxInstructions per
		// frame; past that it is killed, at the same instruction on every
		// build (the count includes nothing that varies by build).
		if ( thread->CountInstruction() ) {
			thread->Runaway( instructionPointer );
			threadDying = true;
			break;
		}

		// next statement
		st = &gameLocal.program.GetStatement( instructionPointer );

		switch( st->op ) {
		case OP_RETURN:
			LeaveFunction( st->a );
			break;

		case OP_THREAD:
			// oax: D3 ran the new thread at once (Start). Here it runs after
			// this one stops, in the same frame: the pump has one thread in
			// flight at a time.
			newThread = new idThread( this, st->a->value.functionPtr, st->b->value.argSize );
			newThread->DelayedStart( 0 );

			// return the thread number to the script
			gameLocal.program.ReturnFloat( newThread->GetThreadNum() );
			PopParms( st->b->value.argSize );
			break;

		case OP_OBJTHREAD:
			var_a = GetVariable( st->a );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				func = obj->GetTypeDef()->GetFunction( st->b->value.virtualFunction );
				assert( st->c->value.argSize == func->parmTotal );
				newThread = new idThread( this, GetEntity( *var_a.entityNumberPtr ), func, func->parmTotal );
				newThread->DelayedStart( 0 );

				// return the thread number to the script
				gameLocal.program.ReturnFloat( newThread->GetThreadNum() );
			} else {
				// return a null thread to the script
				gameLocal.program.ReturnFloat( 0.0f );
			}
			PopParms( st->c->value.argSize );
			break;

		case OP_CALL:
			EnterFunction( st->a->value.functionPtr, false );
			break;

		case OP_EVENTCALL:
			CallEvent( st->a->value.functionPtr, st->b->value.argSize );
			break;

		case OP_OBJECTCALL:	
			var_a = GetVariable( st->a );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				func = obj->GetTypeDef()->GetFunction( st->b->value.virtualFunction );
				EnterFunction( func, false );
			} else {
				// return a 'safe' value
				gameLocal.program.ReturnVector( vec3_zero );
				gameLocal.program.ReturnString( "" );
				PopParms( st->c->value.argSize );
			}
			break;

		case OP_SYSCALL:
			CallSysEvent( st->a->value.functionPtr, st->b->value.argSize );
			break;

		case OP_IFNOT:
			var_a = GetVariable( st->a );
			if ( *var_a.intPtr == 0 ) {
				NextInstruction( instructionPointer + st->b->value.jumpOffset );
			}
			break;

		case OP_IF:
			var_a = GetVariable( st->a );
			if ( *var_a.intPtr != 0 ) {
				NextInstruction( instructionPointer + st->b->value.jumpOffset );
			}
			break;

		case OP_GOTO:
			NextInstruction( instructionPointer + st->a->value.jumpOffset );
			break;

		case OP_ADD_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = *var_a.floatPtr + *var_b.floatPtr;
			break;

		case OP_ADD_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.vectorPtr = *var_a.vectorPtr + *var_b.vectorPtr;
			break;

		case OP_ADD_S:
			SetString( st->c, GetString( st->a ) );
			AppendString( st->c, GetString( st->b ) );
			break;

		case OP_ADD_FS:
			var_a = GetVariable( st->a );
			SetString( st->c, FloatToString( *var_a.floatPtr ) );
			AppendString( st->c, GetString( st->b ) );
			break;

		case OP_ADD_SF:
			var_b = GetVariable( st->b );
			SetString( st->c, GetString( st->a ) );
			AppendString( st->c, FloatToString( *var_b.floatPtr ) );
			break;

		case OP_ADD_VS:
			var_a = GetVariable( st->a );
			SetString( st->c, var_a.vectorPtr->ToString() );
			AppendString( st->c, GetString( st->b ) );
			break;

		case OP_ADD_SV:
			var_b = GetVariable( st->b );
			SetString( st->c, GetString( st->a ) );
			AppendString( st->c, var_b.vectorPtr->ToString() );
			break;

		case OP_SUB_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = *var_a.floatPtr - *var_b.floatPtr;
			break;

		case OP_SUB_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.vectorPtr = *var_a.vectorPtr - *var_b.vectorPtr;
			break;

		case OP_MUL_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = *var_a.floatPtr * *var_b.floatPtr;
			break;

		case OP_MUL_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = *var_a.vectorPtr * *var_b.vectorPtr;
			break;

		case OP_MUL_FV:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.vectorPtr = *var_a.floatPtr * *var_b.vectorPtr;
			break;

		case OP_MUL_VF:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.vectorPtr = *var_a.vectorPtr * *var_b.floatPtr;
			break;

		case OP_DIV_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );

			if ( *var_b.floatPtr == 0.0f ) {
				Warning( "Divide by zero" );
				*var_c.floatPtr = idMath::INFINITY;
			} else {
				*var_c.floatPtr = *var_a.floatPtr / *var_b.floatPtr;
			}
			break;

		case OP_MOD_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable ( st->c );

			if ( *var_b.floatPtr == 0.0f ) {
				Warning( "Divide by zero" );
				*var_c.floatPtr = *var_a.floatPtr;
			} else {
				*var_c.floatPtr = static_cast<int>( *var_a.floatPtr ) % static_cast<int>( *var_b.floatPtr );
			}
			break;

		case OP_BITAND:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = static_cast<int>( *var_a.floatPtr ) & static_cast<int>( *var_b.floatPtr );
			break;

		case OP_BITOR:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = static_cast<int>( *var_a.floatPtr ) | static_cast<int>( *var_b.floatPtr );
			break;

		case OP_GE:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr >= *var_b.floatPtr );
			break;

		case OP_LE:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr <= *var_b.floatPtr );
			break;

		case OP_GT:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr > *var_b.floatPtr );
			break;

		case OP_LT:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr < *var_b.floatPtr );
			break;

		case OP_AND:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr != 0.0f ) && ( *var_b.floatPtr != 0.0f );
			break;

		case OP_AND_BOOLF:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.intPtr != 0 ) && ( *var_b.floatPtr != 0.0f );
			break;

		case OP_AND_FBOOL:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr != 0.0f ) && ( *var_b.intPtr != 0 );
			break;

		case OP_AND_BOOLBOOL:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.intPtr != 0 ) && ( *var_b.intPtr != 0 );
			break;

		case OP_OR:	
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr != 0.0f ) || ( *var_b.floatPtr != 0.0f );
			break;

		case OP_OR_BOOLF:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.intPtr != 0 ) || ( *var_b.floatPtr != 0.0f );
			break;

		case OP_OR_FBOOL:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr != 0.0f ) || ( *var_b.intPtr != 0 );
			break;
			
		case OP_OR_BOOLBOOL:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.intPtr != 0 ) || ( *var_b.intPtr != 0 );
			break;
			
		case OP_NOT_BOOL:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.intPtr == 0 );
			break;

		case OP_NOT_F:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr == 0.0f );
			break;

		case OP_NOT_V:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.vectorPtr == vec3_zero );
			break;

		case OP_NOT_S:
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( strlen( GetString( st->a ) ) == 0 );
			break;

		case OP_NOT_ENT:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( GetEntity( *var_a.entityNumberPtr ) == ENTITY_NONE );
			break;

		case OP_NEG_F:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = -*var_a.floatPtr;
			break;

		case OP_NEG_V:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			*var_c.vectorPtr = -*var_a.vectorPtr;
			break;

		case OP_INT_F:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = static_cast<int>( *var_a.floatPtr );
			break;

		case OP_EQ_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr == *var_b.floatPtr );
			break;

		case OP_EQ_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.vectorPtr == *var_b.vectorPtr );
			break;

		case OP_EQ_S:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( idStr::Cmp( GetString( st->a ), GetString( st->b ) ) == 0 );
			break;

		case OP_EQ_E:
		case OP_EQ_EO:
		case OP_EQ_OE:
		case OP_EQ_OO:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.entityNumberPtr == *var_b.entityNumberPtr );
			break;

		case OP_NE_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.floatPtr != *var_b.floatPtr );
			break;

		case OP_NE_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.vectorPtr != *var_b.vectorPtr );
			break;

		case OP_NE_S:
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( idStr::Cmp( GetString( st->a ), GetString( st->b ) ) != 0 );
			break;

		case OP_NE_E:
		case OP_NE_EO:
		case OP_NE_OE:
		case OP_NE_OO:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ( *var_a.entityNumberPtr != *var_b.entityNumberPtr );
			break;

		case OP_UADD_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.floatPtr += *var_a.floatPtr;
			break;

		case OP_UADD_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.vectorPtr += *var_a.vectorPtr;
			break;

		case OP_USUB_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.floatPtr -= *var_a.floatPtr;
			break;

		case OP_USUB_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.vectorPtr -= *var_a.vectorPtr;
			break;

		case OP_UMUL_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.floatPtr *= *var_a.floatPtr;
			break;

		case OP_UMUL_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.vectorPtr *= *var_a.floatPtr;
			break;

		case OP_UDIV_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );

			if ( *var_a.floatPtr == 0.0f ) {
				Warning( "Divide by zero" );
				*var_b.floatPtr = idMath::INFINITY;
			} else {
				*var_b.floatPtr = *var_b.floatPtr / *var_a.floatPtr;
			}
			break;

		case OP_UDIV_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );

			if ( *var_a.floatPtr == 0.0f ) {
				Warning( "Divide by zero" );
				var_b.vectorPtr->Set( idMath::INFINITY, idMath::INFINITY, idMath::INFINITY );
			} else {
				*var_b.vectorPtr = *var_b.vectorPtr / *var_a.floatPtr;
			}
			break;

		case OP_UMOD_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );

			if ( *var_a.floatPtr == 0.0f ) {
				Warning( "Divide by zero" );
				*var_b.floatPtr = *var_a.floatPtr;
			} else {
				*var_b.floatPtr = static_cast<int>( *var_b.floatPtr ) % static_cast<int>( *var_a.floatPtr );
			}
			break;

		case OP_UOR_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.floatPtr = static_cast<int>( *var_b.floatPtr ) | static_cast<int>( *var_a.floatPtr );
			break;

		case OP_UAND_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.floatPtr = static_cast<int>( *var_b.floatPtr ) & static_cast<int>( *var_a.floatPtr );
			break;

		case OP_UINC_F:
			var_a = GetVariable( st->a );
			( *var_a.floatPtr )++;
			break;

		case OP_UINCP_F:
			var_a = GetVariable( st->a );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				var.bytePtr = &obj->data[ st->b->value.ptrOffset ];
				( *var.floatPtr )++;
			}
			break;

		case OP_UDEC_F:
			var_a = GetVariable( st->a );
			( *var_a.floatPtr )--;
			break;

		case OP_UDECP_F:
			var_a = GetVariable( st->a );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				var.bytePtr = &obj->data[ st->b->value.ptrOffset ];
				( *var.floatPtr )--;
			}
			break;

		case OP_COMP_F:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			*var_c.floatPtr = ~static_cast<int>( *var_a.floatPtr );
			break;

		case OP_STORE_F:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.floatPtr = *var_a.floatPtr;
			break;

		case OP_STORE_ENT:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.entityNumberPtr = *var_a.entityNumberPtr;
			break;

		case OP_STORE_BOOL:	
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.intPtr = *var_a.intPtr;
			break;

		case OP_STORE_OBJENT:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( !obj ) {
				*var_b.entityNumberPtr = 0;
			} else if ( !obj->GetTypeDef()->Inherits( st->b->TypeDef() ) ) {
				//Warning( "object '%s' cannot be converted to '%s'", obj->GetTypeName(), st->b->TypeDef()->Name() );
				*var_b.entityNumberPtr = 0;
			} else {
				*var_b.entityNumberPtr = *var_a.entityNumberPtr;
			}
			break;

		case OP_STORE_OBJ:
		case OP_STORE_ENTOBJ:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.entityNumberPtr = *var_a.entityNumberPtr;
			break;

		case OP_STORE_S:
			SetString( st->b, GetString( st->a ) );
			break;

		case OP_STORE_V:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.vectorPtr = *var_a.vectorPtr;
			break;

		case OP_STORE_FTOS:
			var_a = GetVariable( st->a );
			SetString( st->b, FloatToString( *var_a.floatPtr ) );
			break;

		case OP_STORE_BTOS:
			var_a = GetVariable( st->a );
			SetString( st->b, *var_a.intPtr ? "true" : "false" );
			break;

		case OP_STORE_VTOS:
			var_a = GetVariable( st->a );
			SetString( st->b, var_a.vectorPtr->ToString() );
			break;

		case OP_STORE_FTOBOOL:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			if ( *var_a.floatPtr != 0.0f ) {
				*var_b.intPtr = 1;
			} else {
				*var_b.intPtr = 0;
			}
			break;

		case OP_STORE_BOOLTOF:
			var_a = GetVariable( st->a );
			var_b = GetVariable( st->b );
			*var_b.floatPtr = static_cast<float>( *var_a.intPtr );
			break;

		case OP_STOREP_F:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->floatPtr ) {
				var_a = GetVariable( st->a );
				*var_b.evalPtr->floatPtr = *var_a.floatPtr;
			}
			break;

		case OP_STOREP_ENT:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->entityNumberPtr ) {
				var_a = GetVariable( st->a );
				*var_b.evalPtr->entityNumberPtr = *var_a.entityNumberPtr;
			}
			break;

		case OP_STOREP_FLD:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->intPtr ) {
				var_a = GetVariable( st->a );
				*var_b.evalPtr->intPtr = *var_a.intPtr;
			}
			break;

		case OP_STOREP_BOOL:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->intPtr ) {
				var_a = GetVariable( st->a );
				*var_b.evalPtr->intPtr = *var_a.intPtr;
			}
			break;

		case OP_STOREP_S:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->stringPtr ) {
				idStr::Copynz( var_b.evalPtr->stringPtr, GetString( st->a ), MAX_STRING_LEN );
			}
			break;

		case OP_STOREP_V:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->vectorPtr ) {
				var_a = GetVariable( st->a );
				*var_b.evalPtr->vectorPtr = *var_a.vectorPtr;
			}
			break;
		
		case OP_STOREP_FTOS:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->stringPtr ) {
				var_a = GetVariable( st->a );
				idStr::Copynz( var_b.evalPtr->stringPtr, FloatToString( *var_a.floatPtr ), MAX_STRING_LEN );
			}
			break;

		case OP_STOREP_BTOS:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->stringPtr ) {
				var_a = GetVariable( st->a );
				if ( *var_a.floatPtr != 0.0f ) {
					idStr::Copynz( var_b.evalPtr->stringPtr, "true", MAX_STRING_LEN );
				} else {
					idStr::Copynz( var_b.evalPtr->stringPtr, "false", MAX_STRING_LEN );
				}
			}
			break;

		case OP_STOREP_VTOS:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->stringPtr ) {
				var_a = GetVariable( st->a );
				idStr::Copynz( var_b.evalPtr->stringPtr, var_a.vectorPtr->ToString(), MAX_STRING_LEN );
			}
			break;

		case OP_STOREP_FTOBOOL:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->intPtr ) {
				var_a = GetVariable( st->a );
				if ( *var_a.floatPtr != 0.0f ) {
					*var_b.evalPtr->intPtr = 1;
				} else {
					*var_b.evalPtr->intPtr = 0;
				}
			}
			break;

		case OP_STOREP_BOOLTOF:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->floatPtr ) {
				var_a = GetVariable( st->a );
				*var_b.evalPtr->floatPtr = static_cast<float>( *var_a.intPtr );
			}
			break;

		case OP_STOREP_OBJ:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->entityNumberPtr ) {
				var_a = GetVariable( st->a );
				*var_b.evalPtr->entityNumberPtr = *var_a.entityNumberPtr;
			}
			break;

		case OP_STOREP_OBJENT:
			var_b = GetVariable( st->b );
			if ( var_b.evalPtr && var_b.evalPtr->entityNumberPtr ) {
				var_a = GetVariable( st->a );
				obj = GetScriptObject( *var_a.entityNumberPtr );
				if ( !obj ) {
					*var_b.evalPtr->entityNumberPtr = 0;

				// st->b points to type_pointer, which is just a temporary that gets its type reassigned, so we store the real type in st->c
				// so that we can do a type check during run time since we don't know what type the script object is at compile time because it
				// comes from an entity
				} else if ( !obj->GetTypeDef()->Inherits( st->c->TypeDef() ) ) {
					//Warning( "object '%s' cannot be converted to '%s'", obj->GetTypeName(), st->c->TypeDef()->Name() );
					*var_b.evalPtr->entityNumberPtr = 0;
				} else {
					*var_b.evalPtr->entityNumberPtr = *var_a.entityNumberPtr;
				}
			}
			break;

		case OP_ADDRESS:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				var_c.evalPtr->bytePtr = &obj->data[ st->b->value.ptrOffset ];
			} else {
				var_c.evalPtr->bytePtr = NULL;
			}
			break;

		case OP_INDIRECT_F:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				var.bytePtr = &obj->data[ st->b->value.ptrOffset ];
				*var_c.floatPtr = *var.floatPtr;
			} else {
				*var_c.floatPtr = 0.0f;
			}
			break;

		case OP_INDIRECT_ENT:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				var.bytePtr = &obj->data[ st->b->value.ptrOffset ];
				*var_c.entityNumberPtr = *var.entityNumberPtr;
			} else {
				*var_c.entityNumberPtr = 0;
			}
			break;

		case OP_INDIRECT_BOOL:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				var.bytePtr = &obj->data[ st->b->value.ptrOffset ];
				*var_c.intPtr = *var.intPtr;
			} else {
				*var_c.intPtr = 0;
			}
			break;

		case OP_INDIRECT_S:
			var_a = GetVariable( st->a );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				var.bytePtr = &obj->data[ st->b->value.ptrOffset ];
				SetString( st->c, var.stringPtr );
			} else {
				SetString( st->c, "" );
			}
			break;

		case OP_INDIRECT_V:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( obj ) {
				var.bytePtr = &obj->data[ st->b->value.ptrOffset ];
				*var_c.vectorPtr = *var.vectorPtr;
			} else {
				var_c.vectorPtr->Zero();
			}
			break;

		case OP_INDIRECT_OBJ:
			var_a = GetVariable( st->a );
			var_c = GetVariable( st->c );
			obj = GetScriptObject( *var_a.entityNumberPtr );
			if ( !obj ) {
				*var_c.entityNumberPtr = 0;
			} else {
				var.bytePtr = &obj->data[ st->b->value.ptrOffset ];
				*var_c.entityNumberPtr = *var.entityNumberPtr;
			}
			break;

		case OP_PUSH_F:
			var_a = GetVariable( st->a );
			Push( *var_a.intPtr );
			break;

		case OP_PUSH_FTOS:
			var_a = GetVariable( st->a );
			PushString( FloatToString( *var_a.floatPtr ) );
			break;

		case OP_PUSH_BTOF:
			var_a = GetVariable( st->a );
			floatVal = *var_a.intPtr;
			Push( *reinterpret_cast<int *>( &floatVal ) );
			break;

		case OP_PUSH_FTOB:
			var_a = GetVariable( st->a );
			if ( *var_a.floatPtr != 0.0f ) {
				Push( 1 );
			} else {
				Push( 0 );
			}
			break;

		case OP_PUSH_VTOS:
			var_a = GetVariable( st->a );
			PushString( var_a.vectorPtr->ToString() );
			break;

		case OP_PUSH_BTOS:
			var_a = GetVariable( st->a );
			PushString( *var_a.intPtr ? "true" : "false" );
			break;

		case OP_PUSH_ENT:
			var_a = GetVariable( st->a );
			Push( *var_a.entityNumberPtr );
			break;

		case OP_PUSH_S:
			PushString( GetString( st->a ) );
			break;

		case OP_PUSH_V:
			var_a = GetVariable( st->a );
			Push( *reinterpret_cast<int *>( &var_a.vectorPtr->x ) );
			Push( *reinterpret_cast<int *>( &var_a.vectorPtr->y ) );
			Push( *reinterpret_cast<int *>( &var_a.vectorPtr->z ) );
			break;

		case OP_PUSH_OBJ:
			var_a = GetVariable( st->a );
			Push( *var_a.entityNumberPtr );
			break;

		case OP_PUSH_OBJENT:
			var_a = GetVariable( st->a );
			Push( *var_a.entityNumberPtr );
			break;

		case OP_BREAK:
		case OP_CONTINUE:
		default:
			Error( "Bad opcode %i", st->op );
			break;
		}
	}

	return threadDying;
}
