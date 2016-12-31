/* Execution of byte code produced by bytecomp.el.
   Copyright (C) 1985-1988, 1993, 2000-2016 Free Software Foundation,
   Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>

#include "bytecode.h"
#include "lisp.h"
#include "blockinput.h"
#include "character.h"
#include "buffer.h"
#include "keyboard.h"
#include "syntax.h"
#include "window.h"

/* Work around GCC bug 54561.  */
#if GNUC_PREREQ (4, 3, 0)
# pragma GCC diagnostic ignored "-Wclobbered"
#endif


#ifdef BYTE_CODE_METER

#define METER_2(code1, code2) \
  (*aref_addr (AREF (Vbyte_code_meter, code1), code2))
#define METER_1(code) METER_2 (0, code)

#define METER_CODE(last_code, this_code)				\
{									\
  if (byte_metering_on)							\
    {									\
      if (XFASTINT (METER_1 (this_code)) < MOST_POSITIVE_FIXNUM)	\
        XSETFASTINT (METER_1 (this_code),				\
		     XFASTINT (METER_1 (this_code)) + 1);		\
      if (last_code							\
	  && (XFASTINT (METER_2 (last_code, this_code))			\
	      < MOST_POSITIVE_FIXNUM))					\
        XSETFASTINT (METER_2 (last_code, this_code),			\
		     XFASTINT (METER_2 (last_code, this_code)) + 1);	\
    }									\
}

#endif /* BYTE_CODE_METER */


/* Relocate program counters in the stacks on byte_stack_list.  Called
   when GC has completed.  */

void
relocate_byte_stack (struct byte_stack *stack)
{
  for (; stack; stack = stack->next)
    {
#ifdef HAVE_LIBJIT
      if (!stack->byte_string_start)
	continue;
#endif
      if (stack->byte_string_start != SDATA (stack->byte_string))
	{
	  ptrdiff_t offset = stack->pc - stack->byte_string_start;
	  stack->byte_string_start = SDATA (stack->byte_string);
	  stack->pc = stack->byte_string_start + offset;
	}
    }
}


/* Fetch the next byte from the bytecode stream.  */
#ifdef BYTE_CODE_SAFE
#define FETCH (eassert (stack.byte_string_start == SDATA (stack.byte_string)), *stack.pc++)
#else
#define FETCH *stack.pc++
#endif

/* Fetch two bytes from the bytecode stream and make a 16-bit number
   out of them.  */

#define FETCH2 (op = FETCH, op + (FETCH << 8))

/* Push X onto the execution stack.  The expression X should not
   contain TOP, to avoid competing side effects.  */

#define PUSH(x) (*++top = (x))

/* Pop a value off the execution stack.  */

#define POP (*top--)

/* Discard n values from the execution stack.  */

#define DISCARD(n) (top -= (n))

/* Get the value which is at the top of the execution stack, but don't
   pop it.  */

#define TOP (*top)

#define CHECK_RANGE(ARG)						\
  (BYTE_CODE_SAFE && bytestr_length <= (ARG) ? emacs_abort () : (void) 0)

/* A version of the QUIT macro which makes sure that the stack top is
   set before signaling `quit'.  */
#define BYTE_CODE_QUIT					\
  do {							\
    if (quitcounter++)					\
      break;						\
    maybe_gc ();					\
    if (!NILP (Vquit_flag) && NILP (Vinhibit_quit))	\
      {							\
	Lisp_Object flag = Vquit_flag;			\
	Vquit_flag = Qnil;				\
	if (EQ (Vthrow_on_input, flag))			\
	  Fthrow (Vthrow_on_input, Qt);			\
	quit ();					\
      }							\
    else if (pending_signals)				\
      process_pending_signals ();			\
  } while (0)


DEFUN ("byte-code", Fbyte_code, Sbyte_code, 3, 3, 0,
       doc: /* Function used internally in byte-compiled code.
The first argument, BYTESTR, is a string of byte code;
the second, VECTOR, a vector of constants;
the third, MAXDEPTH, the maximum stack depth used in this function.
If the third argument is incorrect, Emacs may crash.  */)
  (Lisp_Object bytestr, Lisp_Object vector, Lisp_Object maxdepth)
{
  return exec_byte_code__ (bytestr, vector, maxdepth, Qnil, 0, NULL);
}

void
bcall0 (Lisp_Object f)
{
  Ffuncall (1, &f);
}

/* Execute the byte-code in BYTESTR.  VECTOR is the constant vector, and
   MAXDEPTH is the maximum stack depth used (if MAXDEPTH is incorrect,
   emacs may crash!).  If ARGS_TEMPLATE is non-nil, it should be a lisp
   argument list (including &rest, &optional, etc.), and ARGS, of size
   NARGS, should be a vector of the actual arguments.  The arguments in
   ARGS are pushed on the stack according to ARGS_TEMPLATE before
   executing BYTESTR.  */

Lisp_Object
exec_byte_code__ (Lisp_Object bytestr, Lisp_Object vector, Lisp_Object maxdepth,
		  Lisp_Object args_template, ptrdiff_t nargs, Lisp_Object *args)
{
#ifdef BYTE_CODE_METER
  int volatile this_op = 0;
#endif

  CHECK_STRING (bytestr);
  CHECK_VECTOR (vector);
  CHECK_NATNUM (maxdepth);

  ptrdiff_t const_length = ASIZE (vector);

  if (STRING_MULTIBYTE (bytestr))
    /* BYTESTR must have been produced by Emacs 20.2 or the earlier
       because they produced a raw 8-bit string for byte-code and now
       such a byte-code string is loaded as multibyte while raw 8-bit
       characters converted to multibyte form.  Thus, now we must
       convert them back to the originally intended unibyte form.  */
    bytestr = Fstring_as_unibyte (bytestr);

  ptrdiff_t bytestr_length = SBYTES (bytestr);
  Lisp_Object *vectorp = XVECTOR (vector)->contents;
  struct byte_stack stack;

  stack.byte_string = bytestr;
  stack.pc = stack.byte_string_start = SDATA (bytestr);
  unsigned char quitcounter = 0;
  EMACS_INT stack_items = XFASTINT (maxdepth) + 1;
  USE_SAFE_ALLOCA;
  Lisp_Object *stack_base;
  SAFE_ALLOCA_LISP (stack_base, stack_items);
  Lisp_Object *stack_lim = stack_base + stack_items;
  Lisp_Object *top = stack_base;
  stack.next = byte_stack_list;
  byte_stack_list = &stack;
  ptrdiff_t count = SPECPDL_INDEX ();

  if (!NILP (args_template))
    {
      eassert (INTEGERP (args_template));
      ptrdiff_t at = XINT (args_template);
      bool rest = (at & 128) != 0;
      int mandatory = at & 127;
      ptrdiff_t nonrest = at >> 8;
      ptrdiff_t maxargs = rest ? PTRDIFF_MAX : nonrest;
      if (! (mandatory <= nargs && nargs <= maxargs))
	Fsignal (Qwrong_number_of_arguments,
		 list2 (Fcons (make_number (mandatory), make_number (nonrest)),
			make_number (nargs)));
      ptrdiff_t pushedargs = min (nonrest, nargs);
      for (ptrdiff_t i = 0; i < pushedargs; i++, args++)
	PUSH (*args);
      if (nonrest < nargs)
	PUSH (Flist (nargs - nonrest, args));
      else
	for (ptrdiff_t i = nargs - rest; i < nonrest; i++)
	  PUSH (Qnil);
    }

  while (true)
    {
      int op;
      enum handlertype type;

      if (BYTE_CODE_SAFE && ! (stack_base <= top && top < stack_lim))
	emacs_abort ();

#ifdef BYTE_CODE_METER
      int prev_op = this_op;
      this_op = op = FETCH;
      METER_CODE (prev_op, op);
#elif !defined BYTE_CODE_THREADED
      op = FETCH;
#endif

      /* The interpreter can be compiled one of two ways: as an
	 ordinary switch-based interpreter, or as a threaded
	 interpreter.  The threaded interpreter relies on GCC's
	 computed goto extension, so it is not available everywhere.
	 Threading provides a performance boost.  These macros are how
	 we allow the code to be compiled both ways.  */
#ifdef BYTE_CODE_THREADED
      /* The CASE macro introduces an instruction's body.  It is
	 either a label or a case label.  */
#define CASE(OP) insn_ ## OP
      /* NEXT is invoked at the end of an instruction to go to the
	 next instruction.  It is either a computed goto, or a
	 plain break.  */
#define NEXT goto *(targets[op = FETCH])
      /* FIRST is like NEXT, but is only used at the start of the
	 interpreter body.  In the switch-based interpreter it is the
	 switch, so the threaded definition must include a semicolon.  */
#define FIRST NEXT;
      /* Most cases are labeled with the CASE macro, above.
	 CASE_DEFAULT is one exception; it is used if the interpreter
	 being built requires a default case.  The threaded
	 interpreter does not, because the dispatch table is
	 completely filled.  */
#define CASE_DEFAULT
      /* This introduces an instruction that is known to call abort.  */
#define CASE_ABORT CASE (Bstack_ref): CASE (default)
#else
      /* See above for the meaning of the various defines.  */
#define CASE(OP) case OP
#define NEXT break
#define FIRST switch (op)
#define CASE_DEFAULT case 255: default:
#define CASE_ABORT case 0
#endif

#ifdef BYTE_CODE_THREADED

      /* A convenience define that saves us a lot of typing and makes
	 the table clearer.  */
#define LABEL(OP) [OP] = &&insn_ ## OP

#if GNUC_PREREQ (4, 6, 0)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Woverride-init"
#elif defined __clang__
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Winitializer-overrides"
#endif

      /* This is the dispatch table for the threaded interpreter.  */
      static const void *const targets[256] =
	{
	  [0 ... (Bconstant - 1)] = &&insn_default,
	  [Bconstant ... 255] = &&insn_Bconstant,
#define DEFINE(name, value) LABEL (name) ,
	  BYTE_CODES
#undef DEFINE
	};

#if GNUC_PREREQ (4, 6, 0) || defined __clang__
# pragma GCC diagnostic pop
#endif

#endif


      FIRST
	{
	CASE (Bvarref7):
	  op = FETCH2;
	  goto varref;

	CASE (Bvarref):
	CASE (Bvarref1):
	CASE (Bvarref2):
	CASE (Bvarref3):
	CASE (Bvarref4):
	CASE (Bvarref5):
	  op -= Bvarref;
	  goto varref;

	/* This seems to be the most frequently executed byte-code
	   among the Bvarref's, so avoid a goto here.  */
	CASE (Bvarref6):
	  op = FETCH;
	varref:
	  {
	    Lisp_Object v1 = vectorp[op], v2;
	    if (!SYMBOLP (v1)
		|| XSYMBOL (v1)->redirect != SYMBOL_PLAINVAL
		|| (v2 = SYMBOL_VAL (XSYMBOL (v1)), EQ (v2, Qunbound)))
	      v2 = Fsymbol_value (v1);
	    PUSH (v2);
	    NEXT;
	  }

	CASE (Bgotoifnil):
	  {
	    Lisp_Object v1;
	    op = FETCH2;
	    v1 = POP;
	    if (NILP (v1))
	      {
		BYTE_CODE_QUIT;
		CHECK_RANGE (op);
		stack.pc = stack.byte_string_start + op;
	      }
	    NEXT;
	  }

	CASE (Bcar):
	  if (CONSP (TOP))
	    TOP = XCAR (TOP);
	  else if (!NILP (TOP))
	    wrong_type_argument (Qlistp, TOP);
	  NEXT;

	CASE (Beq):
	  {
	    Lisp_Object v1 = POP;
	    TOP = EQ (v1, TOP) ? Qt : Qnil;
	    NEXT;
	  }

	CASE (Bmemq):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fmemq (TOP, v1);
	    NEXT;
	  }

	CASE (Bcdr):
	  {
	    if (CONSP (TOP))
	      TOP = XCDR (TOP);
	    else if (!NILP (TOP))
	      wrong_type_argument (Qlistp, TOP);
	    NEXT;
	  }

	CASE (Bvarset):
	CASE (Bvarset1):
	CASE (Bvarset2):
	CASE (Bvarset3):
	CASE (Bvarset4):
	CASE (Bvarset5):
	  op -= Bvarset;
	  goto varset;

	CASE (Bvarset7):
	  op = FETCH2;
	  goto varset;

	CASE (Bvarset6):
	  op = FETCH;
	varset:
	  {
	    Lisp_Object sym = vectorp[op];
	    Lisp_Object val = POP;

	    /* Inline the most common case.  */
	    if (SYMBOLP (sym)
		&& !EQ (val, Qunbound)
		&& !XSYMBOL (sym)->redirect
		&& !SYMBOL_TRAPPED_WRITE_P (sym))
	      SET_SYMBOL_VAL (XSYMBOL (sym), val);
	    else
              set_internal (sym, val, Qnil, SET_INTERNAL_SET);
	  }
	  NEXT;

	CASE (Bdup):
	  {
	    Lisp_Object v1 = TOP;
	    PUSH (v1);
	    NEXT;
	  }

	/* ------------------ */

	CASE (Bvarbind6):
	  op = FETCH;
	  goto varbind;

	CASE (Bvarbind7):
	  op = FETCH2;
	  goto varbind;

	CASE (Bvarbind):
	CASE (Bvarbind1):
	CASE (Bvarbind2):
	CASE (Bvarbind3):
	CASE (Bvarbind4):
	CASE (Bvarbind5):
	  op -= Bvarbind;
	varbind:
	  /* Specbind can signal and thus GC.  */
	  specbind (vectorp[op], POP);
	  NEXT;

	CASE (Bcall6):
	  op = FETCH;
	  goto docall;

	CASE (Bcall7):
	  op = FETCH2;
	  goto docall;

	CASE (Bcall):
	CASE (Bcall1):
	CASE (Bcall2):
	CASE (Bcall3):
	CASE (Bcall4):
	CASE (Bcall5):
	  op -= Bcall;
	docall:
	  {
	    DISCARD (op);
#ifdef BYTE_CODE_METER
	    if (byte_metering_on && SYMBOLP (TOP))
	      {
		Lisp_Object v1 = TOP;
		Lisp_Object v2 = Fget (v1, Qbyte_code_meter);
		if (INTEGERP (v2)
		    && XINT (v2) < MOST_POSITIVE_FIXNUM)
		  {
		    XSETINT (v2, XINT (v2) + 1);
		    Fput (v1, Qbyte_code_meter, v2);
		  }
	      }
#endif
	    TOP = Ffuncall (op + 1, &TOP);
	    NEXT;
	  }

	CASE (Bunbind6):
	  op = FETCH;
	  goto dounbind;

	CASE (Bunbind7):
	  op = FETCH2;
	  goto dounbind;

	CASE (Bunbind):
	CASE (Bunbind1):
	CASE (Bunbind2):
	CASE (Bunbind3):
	CASE (Bunbind4):
	CASE (Bunbind5):
	  op -= Bunbind;
	dounbind:
	  unbind_to (SPECPDL_INDEX () - op, Qnil);
	  NEXT;

	CASE (Bunbind_all):	/* Obsolete.  Never used.  */
	  /* To unbind back to the beginning of this frame.  Not used yet,
	     but will be needed for tail-recursion elimination.  */
	  unbind_to (count, Qnil);
	  NEXT;

	CASE (Bgoto):
	  BYTE_CODE_QUIT;
	  op = FETCH2;    /* pc = FETCH2 loses since FETCH2 contains pc++ */
	  CHECK_RANGE (op);
	  stack.pc = stack.byte_string_start + op;
	  NEXT;

	CASE (Bgotoifnonnil):
	  op = FETCH2;
	  Lisp_Object v1 = POP;
	  if (!NILP (v1))
	    {
	      BYTE_CODE_QUIT;
	      CHECK_RANGE (op);
	      stack.pc = stack.byte_string_start + op;
	    }
	  NEXT;

	CASE (Bgotoifnilelsepop):
	  op = FETCH2;
	  if (NILP (TOP))
	    {
	      BYTE_CODE_QUIT;
	      CHECK_RANGE (op);
	      stack.pc = stack.byte_string_start + op;
	    }
	  else DISCARD (1);
	  NEXT;

	CASE (Bgotoifnonnilelsepop):
	  op = FETCH2;
	  if (!NILP (TOP))
	    {
	      BYTE_CODE_QUIT;
	      CHECK_RANGE (op);
	      stack.pc = stack.byte_string_start + op;
	    }
	  else DISCARD (1);
	  NEXT;

	CASE (BRgoto):
	  BYTE_CODE_QUIT;
	  stack.pc += (int) *stack.pc - 127;
	  NEXT;

	CASE (BRgotoifnil):
	  if (NILP (POP))
	    {
	      BYTE_CODE_QUIT;
	      stack.pc += (int) *stack.pc - 128;
	    }
	  stack.pc++;
	  NEXT;

	CASE (BRgotoifnonnil):
	  if (!NILP (POP))
	    {
	      BYTE_CODE_QUIT;
	      stack.pc += (int) *stack.pc - 128;
	    }
	  stack.pc++;
	  NEXT;

	CASE (BRgotoifnilelsepop):
	  op = *stack.pc++;
	  if (NILP (TOP))
	    {
	      BYTE_CODE_QUIT;
	      stack.pc += op - 128;
	    }
	  else DISCARD (1);
	  NEXT;

	CASE (BRgotoifnonnilelsepop):
	  op = *stack.pc++;
	  if (!NILP (TOP))
	    {
	      BYTE_CODE_QUIT;
	      stack.pc += op - 128;
	    }
	  else DISCARD (1);
	  NEXT;

	CASE (Breturn):
	  goto exit;

	CASE (Bdiscard):
	  DISCARD (1);
	  NEXT;

	CASE (Bconstant2):
	  PUSH (vectorp[FETCH2]);
	  NEXT;

	CASE (Bsave_excursion):
	  record_unwind_protect (save_excursion_restore,
				 save_excursion_save ());
	  NEXT;

	CASE (Bsave_current_buffer): /* Obsolete since ??.  */
	CASE (Bsave_current_buffer_1):
	  record_unwind_current_buffer ();
	  NEXT;

	CASE (Bsave_window_excursion): /* Obsolete since 24.1.  */
	  {
	    ptrdiff_t count1 = SPECPDL_INDEX ();
	    record_unwind_protect (restore_window_configuration,
				   Fcurrent_window_configuration (Qnil));
	    TOP = Fprogn (TOP);
	    unbind_to (count1, TOP);
	    NEXT;
	  }

	CASE (Bsave_restriction):
	  record_unwind_protect (save_restriction_restore,
				 save_restriction_save ());
	  NEXT;

	CASE (Bcatch):		/* Obsolete since 24.4.  */
	  {
	    Lisp_Object v1 = POP;
	    TOP = internal_catch (TOP, eval_sub, v1);
	    NEXT;
	  }

	CASE (Bpushcatch):	/* New in 24.4.  */
	  type = CATCHER;
	  goto pushhandler;
	CASE (Bpushconditioncase): /* New in 24.4.  */
	  type = CONDITION_CASE;
	pushhandler:
	  {
	    struct handler *c = push_handler (POP, type);
	    c->bytecode_dest = FETCH2;
	    c->bytecode_top = top;

	    if (sys_setjmp (c->jmp))
	      {
		struct handler *c = handlerlist;
		int dest;
		top = c->bytecode_top;
		dest = c->bytecode_dest;
		handlerlist = c->next;
		PUSH (c->val);
		CHECK_RANGE (dest);
		/* Might have been re-set by longjmp!  */
		stack.byte_string_start = SDATA (stack.byte_string);
		stack.pc = stack.byte_string_start + dest;
	      }

	    NEXT;
	  }

	CASE (Bpophandler):	/* New in 24.4.  */
	  handlerlist = handlerlist->next;
	  NEXT;

	CASE (Bunwind_protect):	/* FIXME: avoid closure for lexbind.  */
	  {
	    Lisp_Object handler = POP;
	    /* Support for a function here is new in 24.4.  */
	    record_unwind_protect (FUNCTIONP (handler) ? bcall0 : unwind_body,
				   handler);
	    NEXT;
	  }

	CASE (Bcondition_case):		/* Obsolete since 24.4.  */
	  {
	    Lisp_Object handlers = POP, body = POP;
	    TOP = internal_lisp_condition_case (TOP, body, handlers);
	    NEXT;
	  }

	CASE (Btemp_output_buffer_setup): /* Obsolete since 24.1.  */
	  CHECK_STRING (TOP);
	  temp_output_buffer_setup (SSDATA (TOP));
	  TOP = Vstandard_output;
	  NEXT;

	CASE (Btemp_output_buffer_show): /* Obsolete since 24.1.  */
	  {
	    Lisp_Object v1 = POP;
	    temp_output_buffer_show (TOP);
	    TOP = v1;
	    /* pop binding of standard-output */
	    unbind_to (SPECPDL_INDEX () - 1, Qnil);
	    NEXT;
	  }

	CASE (Bnth):
	  {
	    Lisp_Object v2 = POP, v1 = TOP;
	    CHECK_NUMBER (v1);
	    EMACS_INT n = XINT (v1);
	    immediate_quit = true;
	    while (--n >= 0 && CONSP (v2))
	      v2 = XCDR (v2);
	    immediate_quit = false;
	    TOP = CAR (v2);
	    NEXT;
	  }

	CASE (Bsymbolp):
	  TOP = SYMBOLP (TOP) ? Qt : Qnil;
	  NEXT;

	CASE (Bconsp):
	  TOP = CONSP (TOP) ? Qt : Qnil;
	  NEXT;

	CASE (Bstringp):
	  TOP = STRINGP (TOP) ? Qt : Qnil;
	  NEXT;

	CASE (Blistp):
	  TOP = CONSP (TOP) || NILP (TOP) ? Qt : Qnil;
	  NEXT;

	CASE (Bnot):
	  TOP = NILP (TOP) ? Qt : Qnil;
	  NEXT;

	CASE (Bcons):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fcons (TOP, v1);
	    NEXT;
	  }

	CASE (Blist1):
	  TOP = list1 (TOP);
	  NEXT;

	CASE (Blist2):
	  {
	    Lisp_Object v1 = POP;
	    TOP = list2 (TOP, v1);
	    NEXT;
	  }

	CASE (Blist3):
	  DISCARD (2);
	  TOP = Flist (3, &TOP);
	  NEXT;

	CASE (Blist4):
	  DISCARD (3);
	  TOP = Flist (4, &TOP);
	  NEXT;

	CASE (BlistN):
	  op = FETCH;
	  DISCARD (op - 1);
	  TOP = Flist (op, &TOP);
	  NEXT;

	CASE (Blength):
	  TOP = Flength (TOP);
	  NEXT;

	CASE (Baref):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Faref (TOP, v1);
	    NEXT;
	  }

	CASE (Baset):
	  {
	    Lisp_Object v2 = POP, v1 = POP;
	    TOP = Faset (TOP, v1, v2);
	    NEXT;
	  }

	CASE (Bsymbol_value):
	  TOP = Fsymbol_value (TOP);
	  NEXT;

	CASE (Bsymbol_function):
	  TOP = Fsymbol_function (TOP);
	  NEXT;

	CASE (Bset):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fset (TOP, v1);
	    NEXT;
	  }

	CASE (Bfset):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Ffset (TOP, v1);
	    NEXT;
	  }

	CASE (Bget):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fget (TOP, v1);
	    NEXT;
	  }

	CASE (Bsubstring):
	  {
	    Lisp_Object v2 = POP, v1 = POP;
	    TOP = Fsubstring (TOP, v1, v2);
	    NEXT;
	  }

	CASE (Bconcat2):
	  DISCARD (1);
	  TOP = Fconcat (2, &TOP);
	  NEXT;

	CASE (Bconcat3):
	  DISCARD (2);
	  TOP = Fconcat (3, &TOP);
	  NEXT;

	CASE (Bconcat4):
	  DISCARD (3);
	  TOP = Fconcat (4, &TOP);
	  NEXT;

	CASE (BconcatN):
	  op = FETCH;
	  DISCARD (op - 1);
	  TOP = Fconcat (op, &TOP);
	  NEXT;

	CASE (Bsub1):
	  TOP = INTEGERP (TOP) ? make_number (XINT (TOP) - 1) : Fsub1 (TOP);
	  NEXT;

	CASE (Badd1):
	  TOP = INTEGERP (TOP) ? make_number (XINT (TOP) + 1) : Fadd1 (TOP);
	  NEXT;

	CASE (Beqlsign):
	  {
	    Lisp_Object v2 = POP, v1 = TOP;
	    CHECK_NUMBER_OR_FLOAT_COERCE_MARKER (v1);
	    CHECK_NUMBER_OR_FLOAT_COERCE_MARKER (v2);
	    bool equal;
	    if (FLOATP (v1) || FLOATP (v2))
	      {
		double f1 = FLOATP (v1) ? XFLOAT_DATA (v1) : XINT (v1);
		double f2 = FLOATP (v2) ? XFLOAT_DATA (v2) : XINT (v2);
		equal = f1 == f2;
	      }
	    else
	      equal = XINT (v1) == XINT (v2);
	    TOP = equal ? Qt : Qnil;
	    NEXT;
	  }

	CASE (Bgtr):
	  {
	    Lisp_Object v1 = POP;
	    TOP = arithcompare (TOP, v1, ARITH_GRTR);
	    NEXT;
	  }

	CASE (Blss):
	  {
	    Lisp_Object v1 = POP;
	    TOP = arithcompare (TOP, v1, ARITH_LESS);
	    NEXT;
	  }

	CASE (Bleq):
	  {
	    Lisp_Object v1 = POP;
	    TOP = arithcompare (TOP, v1, ARITH_LESS_OR_EQUAL);
	    NEXT;
	  }

	CASE (Bgeq):
	  {
	    Lisp_Object v1 = POP;
	    TOP = arithcompare (TOP, v1, ARITH_GRTR_OR_EQUAL);
	    NEXT;
	  }

	CASE (Bdiff):
	  DISCARD (1);
	  TOP = Fminus (2, &TOP);
	  NEXT;

	CASE (Bnegate):
	  TOP = INTEGERP (TOP) ? make_number (- XINT (TOP)) : Fminus (1, &TOP);
	  NEXT;

	CASE (Bplus):
	  DISCARD (1);
	  TOP = Fplus (2, &TOP);
	  NEXT;

	CASE (Bmax):
	  DISCARD (1);
	  TOP = Fmax (2, &TOP);
	  NEXT;

	CASE (Bmin):
	  DISCARD (1);
	  TOP = Fmin (2, &TOP);
	  NEXT;

	CASE (Bmult):
	  DISCARD (1);
	  TOP = Ftimes (2, &TOP);
	  NEXT;

	CASE (Bquo):
	  DISCARD (1);
	  TOP = Fquo (2, &TOP);
	  NEXT;

	CASE (Brem):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Frem (TOP, v1);
	    NEXT;
	  }

	CASE (Bpoint):
	  PUSH (make_natnum (PT));
	  NEXT;

	CASE (Bgoto_char):
	  TOP = Fgoto_char (TOP);
	  NEXT;

	CASE (Binsert):
	  TOP = Finsert (1, &TOP);
	  NEXT;

	CASE (BinsertN):
	  op = FETCH;
	  DISCARD (op - 1);
	  TOP = Finsert (op, &TOP);
	  NEXT;

	CASE (Bpoint_max):
	  {
	    Lisp_Object v1;
	    XSETFASTINT (v1, ZV);
	    PUSH (v1);
	    NEXT;
	  }

	CASE (Bpoint_min):
	  PUSH (make_natnum (BEGV));
	  NEXT;

	CASE (Bchar_after):
	  TOP = Fchar_after (TOP);
	  NEXT;

	CASE (Bfollowing_char):
	  PUSH (Ffollowing_char ());
	  NEXT;

	CASE (Bpreceding_char):
	  PUSH (Fprevious_char ());
	  NEXT;

	CASE (Bcurrent_column):
	  PUSH (make_natnum (current_column ()));
	  NEXT;

	CASE (Bindent_to):
	  TOP = Findent_to (TOP, Qnil);
	  NEXT;

	CASE (Beolp):
	  PUSH (Feolp ());
	  NEXT;

	CASE (Beobp):
	  PUSH (Feobp ());
	  NEXT;

	CASE (Bbolp):
	  PUSH (Fbolp ());
	  NEXT;

	CASE (Bbobp):
	  PUSH (Fbobp ());
	  NEXT;

	CASE (Bcurrent_buffer):
	  PUSH (Fcurrent_buffer ());
	  NEXT;

	CASE (Bset_buffer):
	  TOP = Fset_buffer (TOP);
	  NEXT;

	CASE (Binteractive_p):	/* Obsolete since 24.1.  */
	  PUSH (call0 (intern ("interactive-p")));
	  NEXT;

	CASE (Bforward_char):
	  TOP = Fforward_char (TOP);
	  NEXT;

	CASE (Bforward_word):
	  TOP = Fforward_word (TOP);
	  NEXT;

	CASE (Bskip_chars_forward):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fskip_chars_forward (TOP, v1);
	    NEXT;
	  }

	CASE (Bskip_chars_backward):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fskip_chars_backward (TOP, v1);
	    NEXT;
	  }

	CASE (Bforward_line):
	  TOP = Fforward_line (TOP);
	  NEXT;

	CASE (Bchar_syntax):
	  {
	    CHECK_CHARACTER (TOP);
	    int c = XFASTINT (TOP);
	    if (NILP (BVAR (current_buffer, enable_multibyte_characters)))
	      MAKE_CHAR_MULTIBYTE (c);
	    XSETFASTINT (TOP, syntax_code_spec[SYNTAX (c)]);
	  }
	  NEXT;

	CASE (Bbuffer_substring):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fbuffer_substring (TOP, v1);
	    NEXT;
	  }

	CASE (Bdelete_region):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fdelete_region (TOP, v1);
	    NEXT;
	  }

	CASE (Bnarrow_to_region):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fnarrow_to_region (TOP, v1);
	    NEXT;
	  }

	CASE (Bwiden):
	  PUSH (Fwiden ());
	  NEXT;

	CASE (Bend_of_line):
	  TOP = Fend_of_line (TOP);
	  NEXT;

	CASE (Bset_marker):
	  {
	    Lisp_Object v2 = POP, v1 = POP;
	    TOP = Fset_marker (TOP, v1, v2);
	    NEXT;
	  }

	CASE (Bmatch_beginning):
	  TOP = Fmatch_beginning (TOP);
	  NEXT;

	CASE (Bmatch_end):
	  TOP = Fmatch_end (TOP);
	  NEXT;

	CASE (Bupcase):
	  TOP = Fupcase (TOP);
	  NEXT;

	CASE (Bdowncase):
	  TOP = Fdowncase (TOP);
	NEXT;

      CASE (Bstringeqlsign):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fstring_equal (TOP, v1);
	    NEXT;
	  }

	CASE (Bstringlss):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fstring_lessp (TOP, v1);
	    NEXT;
	  }

	CASE (Bequal):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fequal (TOP, v1);
	    NEXT;
	  }

	CASE (Bnthcdr):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fnthcdr (TOP, v1);
	    NEXT;
	  }

	CASE (Belt):
	  {
	    if (CONSP (TOP))
	      {
		/* Exchange args and then do nth.  */
		Lisp_Object v2 = POP, v1 = TOP;
		CHECK_NUMBER (v2);
		EMACS_INT n = XINT (v2);
		immediate_quit = true;
		while (--n >= 0 && CONSP (v1))
		  v1 = XCDR (v1);
		immediate_quit = false;
		TOP = CAR (v1);
	      }
	    else
	      {
		Lisp_Object v1 = POP;
		TOP = Felt (TOP, v1);
	      }
	    NEXT;
	  }

	CASE (Bmember):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fmember (TOP, v1);
	    NEXT;
	  }

	CASE (Bassq):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fassq (TOP, v1);
	    NEXT;
	  }

	CASE (Bnreverse):
	  TOP = Fnreverse (TOP);
	  NEXT;

	CASE (Bsetcar):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fsetcar (TOP, v1);
	    NEXT;
	  }

	CASE (Bsetcdr):
	  {
	    Lisp_Object v1 = POP;
	    TOP = Fsetcdr (TOP, v1);
	    NEXT;
	  }

	CASE (Bcar_safe):
	  TOP = CAR_SAFE (TOP);
	  NEXT;

	CASE (Bcdr_safe):
	  TOP = CDR_SAFE (TOP);
	  NEXT;

	CASE (Bnconc):
	  DISCARD (1);
	  TOP = Fnconc (2, &TOP);
	  NEXT;

	CASE (Bnumberp):
	  TOP = NUMBERP (TOP) ? Qt : Qnil;
	  NEXT;

	CASE (Bintegerp):
	  TOP = INTEGERP (TOP) ? Qt : Qnil;
	  NEXT;

#if BYTE_CODE_SAFE
	  /* These are intentionally written using 'case' syntax,
	     because they are incompatible with the threaded
	     interpreter.  */

	case Bset_mark:
	  error ("set-mark is an obsolete bytecode");
	  break;
	case Bscan_buffer:
	  error ("scan-buffer is an obsolete bytecode");
	  break;
#endif

	CASE_ABORT:
	  /* Actually this is Bstack_ref with offset 0, but we use Bdup
	     for that instead.  */
	  /* CASE (Bstack_ref): */
	  call3 (Qerror,
		 build_string ("Invalid byte opcode: op=%s, ptr=%d"),
		 make_number (op),
		 make_number (stack.pc - 1 - stack.byte_string_start));

	  /* Handy byte-codes for lexical binding.  */
	CASE (Bstack_ref1):
	CASE (Bstack_ref2):
	CASE (Bstack_ref3):
	CASE (Bstack_ref4):
	CASE (Bstack_ref5):
	  {
	    Lisp_Object v1 = top[Bstack_ref - op];
	    PUSH (v1);
	    NEXT;
	  }
	CASE (Bstack_ref6):
	  {
	    Lisp_Object v1 = top[- FETCH];
	    PUSH (v1);
	    NEXT;
	  }
	CASE (Bstack_ref7):
	  {
	    Lisp_Object v1 = top[- FETCH2];
	    PUSH (v1);
	    NEXT;
	  }
	CASE (Bstack_set):
	  /* stack-set-0 = discard; stack-set-1 = discard-1-preserve-tos.  */
	  {
	    Lisp_Object *ptr = top - FETCH;
	    *ptr = POP;
	    NEXT;
	  }
	CASE (Bstack_set2):
	  {
	    Lisp_Object *ptr = top - FETCH2;
	    *ptr = POP;
	    NEXT;
	  }
	CASE (BdiscardN):
	  op = FETCH;
	  if (op & 0x80)
	    {
	      op &= 0x7F;
	      top[-op] = TOP;
	    }
	  DISCARD (op);
	  NEXT;

	CASE_DEFAULT
	CASE (Bconstant):
	  if (BYTE_CODE_SAFE
	      && ! (Bconstant <= op && op < Bconstant + const_length))
	    emacs_abort ();
	  PUSH (vectorp[op - Bconstant]);
	  NEXT;
	}
    }

 exit:

  byte_stack_list = byte_stack_list->next;

  /* Binds and unbinds are supposed to be compiled balanced.  */
  if (SPECPDL_INDEX () != count)
    {
      if (SPECPDL_INDEX () > count)
	unbind_to (count, Qnil);
      error ("binding stack not balanced (serious byte compiler bug)");
    }

  Lisp_Object result = TOP;
  SAFE_FREE ();
  return result;
}

Lisp_Object
exec_byte_code (Lisp_Object byte_code, Lisp_Object args_template,
		ptrdiff_t nargs, Lisp_Object *args)
{
#ifdef HAVE_LIBJIT
  if (AREF (byte_code, COMPILED_JIT_ID))
    return jit_exec (byte_code, args_template, nargs, args);
  else if (byte_code_jit_on)
    {
      jit_byte_code__ (byte_code);
      return jit_exec (byte_code, args_template, nargs, args);
    }
  else
#endif
    return exec_byte_code__ (AREF (byte_code, COMPILED_BYTECODE),
			     AREF (byte_code, COMPILED_CONSTANTS),
			     AREF (byte_code, COMPILED_STACK_DEPTH),
			     args_template, nargs, args);
}


/* `args_template' has the same meaning as in exec_byte_code() above.  */
Lisp_Object
get_byte_code_arity (Lisp_Object args_template)
{
  eassert (NATNUMP (args_template));
  EMACS_INT at = XINT (args_template);
  bool rest = (at & 128) != 0;
  int mandatory = at & 127;
  EMACS_INT nonrest = at >> 8;

  return Fcons (make_number (mandatory),
		rest ? Qmany : make_number (nonrest));
}

void
syms_of_bytecode (void)
{
  defsubr (&Sbyte_code);

#ifdef BYTE_CODE_METER

  DEFVAR_LISP ("byte-code-meter", Vbyte_code_meter,
	       doc: /* A vector of vectors which holds a histogram of byte-code usage.
\(aref (aref byte-code-meter 0) CODE) indicates how many times the byte
opcode CODE has been executed.
\(aref (aref byte-code-meter CODE1) CODE2), where CODE1 is not 0,
indicates how many times the byte opcodes CODE1 and CODE2 have been
executed in succession.  */);

  DEFVAR_BOOL ("byte-metering-on", byte_metering_on,
	       doc: /* If non-nil, keep profiling information on byte code usage.
The variable byte-code-meter indicates how often each byte opcode is used.
If a symbol has a property named `byte-code-meter' whose value is an
integer, it is incremented each time that symbol's function is called.  */);

  byte_metering_on = false;
  Vbyte_code_meter = Fmake_vector (make_number (256), make_number (0));
  DEFSYM (Qbyte_code_meter, "byte-code-meter");
  {
    int i = 256;
    while (i--)
      ASET (Vbyte_code_meter, i,
           Fmake_vector (make_number (256), make_number (0)));
  }
#endif
}
