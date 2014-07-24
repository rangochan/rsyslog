/* A Bison parser, made by GNU Bison 2.7.  */

/* Bison interface for Yacc-like parsers in C
   
      Copyright (C) 1984, 1989-1990, 2000-2012 Free Software Foundation, Inc.
   
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.
   
   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

#ifndef YY_YY_GRAMMAR_H_INCLUDED
# define YY_YY_GRAMMAR_H_INCLUDED
/* Enabling traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     NAME = 258,
     FUNC = 259,
     BEGINOBJ = 260,
     ENDOBJ = 261,
     BEGIN_ACTION = 262,
     BEGIN_PROPERTY = 263,
     BEGIN_CONSTANT = 264,
     BEGIN_TPL = 265,
     BEGIN_RULESET = 266,
     STOP = 267,
     SET = 268,
     UNSET = 269,
     CONTINUE = 270,
     CALL = 271,
     LEGACY_ACTION = 272,
     LEGACY_RULESET = 273,
     PRIFILT = 274,
     PROPFILT = 275,
     BSD_TAG_SELECTOR = 276,
     BSD_HOST_SELECTOR = 277,
     IF = 278,
     THEN = 279,
     ELSE = 280,
     OR = 281,
     AND = 282,
     NOT = 283,
     VAR = 284,
     STRING = 285,
     NUMBER = 286,
     CMP_EQ = 287,
     CMP_NE = 288,
     CMP_LE = 289,
     CMP_GE = 290,
     CMP_LT = 291,
     CMP_GT = 292,
     CMP_CONTAINS = 293,
     CMP_CONTAINSI = 294,
     CMP_STARTSWITH = 295,
     CMP_STARTSWITHI = 296,
     UMINUS = 297
   };
#endif
/* Tokens.  */
#define NAME 258
#define FUNC 259
#define BEGINOBJ 260
#define ENDOBJ 261
#define BEGIN_ACTION 262
#define BEGIN_PROPERTY 263
#define BEGIN_CONSTANT 264
#define BEGIN_TPL 265
#define BEGIN_RULESET 266
#define STOP 267
#define SET 268
#define UNSET 269
#define CONTINUE 270
#define CALL 271
#define LEGACY_ACTION 272
#define LEGACY_RULESET 273
#define PRIFILT 274
#define PROPFILT 275
#define BSD_TAG_SELECTOR 276
#define BSD_HOST_SELECTOR 277
#define IF 278
#define THEN 279
#define ELSE 280
#define OR 281
#define AND 282
#define NOT 283
#define VAR 284
#define STRING 285
#define NUMBER 286
#define CMP_EQ 287
#define CMP_NE 288
#define CMP_LE 289
#define CMP_GE 290
#define CMP_LT 291
#define CMP_GT 292
#define CMP_CONTAINS 293
#define CMP_CONTAINSI 294
#define CMP_STARTSWITH 295
#define CMP_STARTSWITHI 296
#define UMINUS 297



#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
{
/* Line 2058 of yacc.c  */
#line 43 "grammar.y"

	char *s;
	long long n;
	es_str_t *estr;
	enum cnfobjType objType;
	struct cnfobj *obj;
	struct cnfstmt *stmt;
	struct nvlst *nvlst;
	struct objlst *objlst;
	struct cnfexpr *expr;
	struct cnfarray *arr;
	struct cnffunc *func;
	struct cnffparamlst *fparams;


/* Line 2058 of yacc.c  */
#line 157 "grammar.h"
} YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
#endif

extern YYSTYPE yylval;

#ifdef YYPARSE_PARAM
#if defined __STDC__ || defined __cplusplus
int yyparse (void *YYPARSE_PARAM);
#else
int yyparse ();
#endif
#else /* ! YYPARSE_PARAM */
#if defined __STDC__ || defined __cplusplus
int yyparse (void);
#else
int yyparse ();
#endif
#endif /* ! YYPARSE_PARAM */

#endif /* !YY_YY_GRAMMAR_H_INCLUDED  */
