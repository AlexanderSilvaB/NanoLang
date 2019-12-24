#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "common.h"
#include "compiler.h"
#include "memory.h"
#include "scanner.h"
#include "util.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct
{
  Token current;
  Token previous;
  bool hadError;
  bool panicMode;
} Parser;

typedef enum
{
  PREC_NONE,
  PREC_ASSIGNMENT, // =
  PREC_OR,         // or
  PREC_AND,        // and
  PREC_EQUALITY,   // == !=
  PREC_COMPARISON, // < > <= >=
  PREC_TERM,       // + - in is
  PREC_FACTOR,     // * /
  PREC_UNARY,      // ! -
  PREC_CALL,       // . ()
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct
{
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

typedef struct
{
  Token name;
  int depth;
  bool isCaptured;
} Local;

typedef struct
{
  uint8_t index;
  bool isLocal;
} Upvalue;

typedef enum
{
  TYPE_FUNCTION,
  TYPE_INITIALIZER,
  TYPE_METHOD,
  TYPE_STATIC,
  TYPE_SCRIPT
} FunctionType;

typedef struct Compiler
{
  struct Compiler *enclosing;
  ObjFunction *function;
  FunctionType type;

  Local locals[UINT8_COUNT];
  int localCount;
  Upvalue upvalues[UINT8_COUNT];
  int scopeDepth;
  int loopDepth;
} Compiler;

typedef struct ClassCompiler
{
  struct ClassCompiler *enclosing;

  Token name;
  bool hasSuperclass;
} ClassCompiler;

typedef struct NamespaceCompiler
{
  struct NamespaceCompiler *enclosing;

  Token name;
} NamespaceCompiler;

Parser parser;

Compiler *current = NULL;

ClassCompiler *currentClass = NULL;
bool staticMethod = false;
NamespaceCompiler *currentNamespace = NULL;

static char *initString = "<CUBE>";

// Used for "continue" statements
int innermostLoopStart = -1;
int innermostLoopScopeDepth = 0;

static Chunk *currentChunk()
{
  return &current->function->chunk;
}

static void errorAt(Token *token, const char *message)
{
  if (parser.panicMode)
    return;
  parser.panicMode = true;

  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF)
  {
    fprintf(stderr, " at end");
  }
  else if (token->type == TOKEN_ERROR)
  {
    // Nothing.
  }
  else
  {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

static void error(const char *message)
{
  errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char *message)
{
  errorAt(&parser.current, message);
}

static void advance()
{
  parser.previous = parser.current;

  for (;;)
  {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR)
      break;

    errorAtCurrent(parser.current.start);
  }
}

static void consume(TokenType type, const char *message)
{
  if (parser.current.type == type)
  {
    advance();
    return;
  }

  errorAtCurrent(message);
}

static bool check(TokenType type)
{
  return parser.current.type == type;
}

static bool match(TokenType type)
{
  if (!check(type))
    return false;
  advance();
  return true;
}

static void emitByte(uint8_t byte)
{
  writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2)
{
  emitByte(byte1);
  emitByte(byte2);
}

static void emitLoop(int loopStart)
{
  emitByte(OP_LOOP);

  int offset = currentChunk()->count - loopStart + 2;
  if (offset > UINT16_MAX)
    error("Loop body too large.");

  emitByte((offset >> 8) & 0xff);
  emitByte(offset & 0xff);
}

static int emitJump(uint8_t instruction)
{
  emitByte(instruction);
  emitByte(0xff);
  emitByte(0xff);
  return currentChunk()->count - 2;
}

static void emitReturn()
{
  if (current->type == TYPE_INITIALIZER)
  {
    emitBytes(OP_GET_LOCAL, 0);
  }
  else
  {
    emitByte(OP_NONE);
  }

  emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value)
{
  int constant = addConstant(currentChunk(), value);
  if (constant > UINT8_MAX)
  {
    error("Too many constants in one chunk.");
    return 0;
  }

  return (uint8_t)constant;
}

static void emitConstant(Value value)
{
  emitBytes(OP_CONSTANT, makeConstant(value));
}

static void patchJump(int offset)
{
  int jump = currentChunk()->count - offset - 2;

  if (jump > UINT16_MAX)
  {
    error("Too much code to jump over.");
  }

  currentChunk()->code[offset] = (jump >> 8) & 0xff;
  currentChunk()->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler *compiler, FunctionType type)
{
  compiler->enclosing = current;
  compiler->function = NULL;
  compiler->type = type;
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  compiler->loopDepth = 0;
  compiler->function = newFunction(type == TYPE_STATIC);
  current = compiler;

  if (type != TYPE_SCRIPT)
  {
    current->function->name = copyString(parser.previous.start,
                                         parser.previous.length);
  }

  Local *local = &current->locals[current->localCount++];
  local->depth = 0;
  local->isCaptured = false;
  if (type != TYPE_FUNCTION && type != TYPE_STATIC)
  {
    local->name.start = "this";
    local->name.length = 4;
  }
  else
  {
    local->name.start = "";
    local->name.length = 0;
  }
}

static ObjFunction *endCompiler()
{
  emitReturn();
  ObjFunction *function = current->function;

#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError)
  {
    disassembleChunk(currentChunk(),
                     function->name != NULL ? function->name->chars : "<script>");
  }
#endif

  current = current->enclosing;
  return function;
}

static void beginScope()
{
  current->scopeDepth++;
}

static void endScope()
{
  current->scopeDepth--;

  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth >
             current->scopeDepth)
  {
    if (current->locals[current->localCount - 1].isCaptured)
    {
      emitByte(OP_CLOSE_UPVALUE);
    }
    else
    {
      emitByte(OP_POP);
    }
    current->localCount--;
  }
}

static void expression();
static void statement();
static void declaration(bool checkEnd);
static ParseRule *getRule(TokenType type);
static void parsePrecedence(Precedence precedence);

static uint8_t identifierConstant(Token *name)
{
  return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token *a, Token *b)
{
  if (a->length != b->length)
    return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler *compiler, Token *name, bool inFunction)
{
  for (int i = compiler->localCount - 1; i >= 0; i--)
  {
    Local *local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name))
    {
      if (!inFunction && local->depth == -1)
      {
        error("Cannot read local variable in its own initializer.");
      }
      return i;
    }
  }

  return -1;
}

static int addUpvalue(Compiler *compiler, uint8_t index, bool isLocal)
{
  int upvalueCount = compiler->function->upvalueCount;

  for (int i = 0; i < upvalueCount; i++)
  {
    Upvalue *upvalue = &compiler->upvalues[i];
    if (upvalue->index == index && upvalue->isLocal == isLocal)
    {
      return i;
    }
  }

  if (upvalueCount == UINT8_COUNT)
  {
    error("Too many closure variables in function.");
    return 0;
  }

  compiler->upvalues[upvalueCount].isLocal = isLocal;
  compiler->upvalues[upvalueCount].index = index;
  return compiler->function->upvalueCount++;
}

static int resolveUpvalue(Compiler *compiler, Token *name)
{
  if (compiler->enclosing == NULL)
    return -1;

  int local = resolveLocal(compiler->enclosing, name, true);
  if (local != -1)
  {
    compiler->enclosing->locals[local].isCaptured = true;
    return addUpvalue(compiler, (uint8_t)local, true);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name);
  if (upvalue != -1)
  {
    return addUpvalue(compiler, (uint8_t)upvalue, false);
  }

  return -1;
}

static void addLocal(Token name)
{
  if (current->localCount == UINT8_COUNT)
  {
    error("Too many local variables in function.");
    return;
  }

  Local *local = &current->locals[current->localCount++];
  local->name = name;
  local->depth = -1;
  local->isCaptured = false;
}

static void declareVariable()
{
  if (current->scopeDepth == 0)
    return;

  Token *name = &parser.previous;
  for (int i = current->localCount - 1; i >= 0; i--)
  {
    Local *local = &current->locals[i];
    if (local->depth != -1 && local->depth < current->scopeDepth)
    {
      break; // [negative]
    }

    if (identifiersEqual(name, &local->name))
    {
      error("Variable with this name already declared in this scope.");
    }
  }

  addLocal(*name);
}

static void declareNamedVariable(Token *name)
{
  if (current->scopeDepth == 0)
    return;

  for (int i = current->localCount - 1; i >= 0; i--)
  {
    Local *local = &current->locals[i];
    if (local->depth != -1 && local->depth < current->scopeDepth)
    {
      break; // [negative]
    }

    if (identifiersEqual(name, &local->name))
    {
      error("Variable with this name already declared in this scope.");
    }
  }

  addLocal(*name);
}

static uint8_t parseVariable(const char *errorMessage)
{
  consume(TOKEN_IDENTIFIER, errorMessage);

  declareVariable();
  if (current->scopeDepth > 0)
    return 0;

  return identifierConstant(&parser.previous);
}

static void markInitialized()
{
  if (current->scopeDepth == 0)
    return;
  current->locals[current->localCount - 1].depth =
      current->scopeDepth;
}

static void defineVariable(uint8_t global)
{
  if (current->scopeDepth > 0)
  {
    markInitialized();
    return;
  }

  emitBytes(OP_DEFINE_GLOBAL, global);
}

static uint8_t argumentList()
{
  uint8_t argCount = 0;
  if (!check(TOKEN_RIGHT_PAREN))
  {
    do
    {
      expression();

      if (argCount == 255)
      {
        error("Cannot have more than 255 arguments.");
      }
      argCount++;
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
  return argCount;
}

static void and_(bool canAssign)
{
  int endJump = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP);
  parsePrecedence(PREC_AND);

  patchJump(endJump);
}

static void is(bool canAssign)
{
  if (match(TOKEN_BANG))
  {
    emitByte(OP_TRUE);
  }
  else
  {
    emitByte(OP_FALSE);
  }

  if (match(TOKEN_IDENTIFIER) || match(TOKEN_NONE) || match(TOKEN_FUNC) || match(TOKEN_CLASS))
  {
    ObjString *str = copyString(parser.previous.start, parser.previous.length);

    if (!isValidType(str->chars))
    {
      errorAtCurrent("Invalid type on 'is' operator.");
    }
    else
    {
      emitConstant(OBJ_VAL(str));
      emitByte(OP_IS);
    }
  }
  else
    errorAtCurrent("Expected type name after 'is'.");
}

static void binary(bool canAssign)
{
  TokenType operatorType = parser.previous.type;

  ParseRule *rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  switch (operatorType)
  {
  case TOKEN_BANG_EQUAL:
    emitBytes(OP_EQUAL, OP_NOT);
    break;
  case TOKEN_EQUAL_EQUAL:
    emitByte(OP_EQUAL);
    break;
  case TOKEN_GREATER:
    emitByte(OP_GREATER);
    break;
  case TOKEN_GREATER_EQUAL:
    emitBytes(OP_LESS, OP_NOT);
    break;
  case TOKEN_LESS:
    emitByte(OP_LESS);
    break;
  case TOKEN_LESS_EQUAL:
    emitBytes(OP_GREATER, OP_NOT);
    break;
  case TOKEN_PLUS:
    emitByte(OP_ADD);
    break;
  case TOKEN_MINUS:
    emitByte(OP_SUBTRACT);
    break;
  case TOKEN_STAR:
    emitByte(OP_MULTIPLY);
    break;
  case TOKEN_SLASH:
    emitByte(OP_DIVIDE);
    break;
  case TOKEN_PERCENT:
    emitByte(OP_MOD);
    break;
  case TOKEN_POW:
    emitByte(OP_POW);
    break;
  case TOKEN_IN:
    emitByte(OP_IN);
    break;
  default:
    return; // Unreachable.
  }
}

static void call(bool canAssign)
{
  uint8_t argCount = argumentList();
  emitBytes(OP_CALL, argCount);
}

static void dot(bool canAssign)
{
  consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
  uint8_t name = identifierConstant(&parser.previous);

  if (canAssign && match(TOKEN_PLUS_EQUALS))
  {
    emitBytes(OP_GET_PROPERTY_NO_POP, name);
    expression();
    emitByte(OP_ADD);
    emitBytes(OP_SET_PROPERTY, name);
  }
  else if (canAssign && match(TOKEN_MINUS_EQUALS))
  {
    emitBytes(OP_GET_PROPERTY_NO_POP, name);
    expression();
    emitByte(OP_SUBTRACT);
    emitBytes(OP_SET_PROPERTY, name);
  }
  else if (canAssign && match(TOKEN_MULTIPLY_EQUALS))
  {
    emitBytes(OP_GET_PROPERTY_NO_POP, name);
    expression();
    emitByte(OP_MULTIPLY);
    emitBytes(OP_SET_PROPERTY, name);
  }
  else if (canAssign && match(TOKEN_DIVIDE_EQUALS))
  {
    emitBytes(OP_GET_PROPERTY_NO_POP, name);
    expression();
    emitByte(OP_DIVIDE);
    emitBytes(OP_SET_PROPERTY, name);
  }
  else if (canAssign && match(TOKEN_EQUAL))
  {
    expression();
    emitBytes(OP_SET_PROPERTY, name);
  }
  else if (match(TOKEN_LEFT_PAREN))
  {
    uint8_t argCount = argumentList();
    emitBytes(OP_INVOKE, argCount);
    emitByte(name);
  }
  else
  {
    emitBytes(OP_GET_PROPERTY, name);
  }
}

static void literal(bool canAssign)
{
  switch (parser.previous.type)
  {
  case TOKEN_FALSE:
    emitByte(OP_FALSE);
    break;
  case TOKEN_NONE:
    emitByte(OP_NONE);
    break;
  case TOKEN_TRUE:
    emitByte(OP_TRUE);
    break;
  default:
    return; // Unreachable.
  }
}

static void grouping(bool canAssign)
{
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign)
{
  double value;
  if (parser.previous.type == TOKEN_NAN)
    value = NAN;
  else if (parser.previous.type == TOKEN_INF)
    value = INFINITY;
  else
    value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void byte(bool canAssign)
{
  int value = strtol(parser.previous.start, NULL, 0);
  int sz = countBytes(&value, sizeof(int));
  emitConstant(BYTES_VAL(&value, sz));
}

static void or_(bool canAssign)
{
  int elseJump = emitJump(OP_JUMP_IF_FALSE);
  int endJump = emitJump(OP_JUMP);

  patchJump(elseJump);
  emitByte(OP_POP);

  parsePrecedence(PREC_OR);
  patchJump(endJump);
}

static void string(bool canAssign)
{
  char *str = malloc(sizeof(char) * parser.previous.length);
  int j = 0;
  for (int i = 1; i < parser.previous.length - 1; i++)
  {
    if (parser.previous.start[i] == '\\' && i < parser.previous.length - 2)
    {
      if (parser.previous.start[i + 1] == 'n')
      {
        i++;
        str[j++] = '\n';
      }
      else if (parser.previous.start[i + 1] == 't')
      {
        i++;
        str[j++] = '\t';
      }
      else if (parser.previous.start[i + 1] == 'b')
      {
        i++;
        str[j++] = '\b';
      }
      else if (parser.previous.start[i + 1] == 'v')
      {
        i++;
        str[j++] = '\v';
      }
      else
      {
        str[j++] == '\\';
      }
    }
    else
      str[j++] = parser.previous.start[i];
  }

  str[j] = '\0';

  emitConstant(STRING_VAL(str));
  free(str);
}

static void list(bool canAssign)
{
  emitByte(OP_NEW_LIST);

  do
  {
    if (check(TOKEN_RIGHT_BRACKET))
      break;

    expression();
    emitByte(OP_ADD_LIST);
  } while (match(TOKEN_COMMA));

  consume(TOKEN_RIGHT_BRACKET, "Expected closing ']'");
}

static void dict(bool canAssign)
{
  emitByte(OP_NEW_DICT);

  do
  {
    if (check(TOKEN_RIGHT_BRACE))
      break;

    parsePrecedence(PREC_UNARY);
    consume(TOKEN_COLON, "Expected ':'");
    expression();
    emitByte(OP_ADD_DICT);
  } while (match(TOKEN_COMMA));

  consume(TOKEN_RIGHT_BRACE, "Expected closing '}'");
}

static void subscript(bool canAssign)
{
  expression();

  consume(TOKEN_RIGHT_BRACKET, "Expected closing ']'");

  if (match(TOKEN_EQUAL))
  {
    expression();
    emitByte(OP_SUBSCRIPT_ASSIGN);
  }
  else
    emitByte(OP_SUBSCRIPT);
}

static uint8_t setVariablePop(Token name)
{
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name, false);
  if (arg != -1)
  {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  }
  else if ((arg = resolveUpvalue(current, &name)) != -1)
  {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  }
  else
  {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  emitBytes(setOp, (uint8_t)arg);
  return (uint8_t)arg;
}

static uint8_t setVariable(Token name, Value value)
{
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name, false);
  if (arg != -1)
  {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  }
  else if ((arg = resolveUpvalue(current, &name)) != -1)
  {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  }
  else
  {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  emitConstant(value);
  emitBytes(setOp, (uint8_t)arg);
  return (uint8_t)arg;
}

static uint8_t getVariable(Token name)
{
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name, false);
  if (arg != -1)
  {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  }
  else if ((arg = resolveUpvalue(current, &name)) != -1)
  {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  }
  else
  {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  emitBytes(getOp, (uint8_t)arg);
  return (uint8_t)arg;
}

static void namedVariable(Token name, bool canAssign)
{
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name, false);
  if (arg != -1)
  {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  }
  else if ((arg = resolveUpvalue(current, &name)) != -1)
  {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  }
  else
  {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  if (canAssign && match(TOKEN_PLUS_EQUALS))
  {
    namedVariable(name, false);
    expression();
    emitByte(OP_ADD);
    emitBytes(setOp, (uint8_t)arg);
  }
  else if (canAssign && match(TOKEN_MINUS_EQUALS))
  {
    namedVariable(name, false);
    expression();
    emitByte(OP_SUBTRACT);
    emitBytes(setOp, (uint8_t)arg);
  }
  else if (canAssign && match(TOKEN_MULTIPLY_EQUALS))
  {
    namedVariable(name, false);
    expression();
    emitByte(OP_MULTIPLY);
    emitBytes(setOp, (uint8_t)arg);
  }
  else if (canAssign && match(TOKEN_DIVIDE_EQUALS))
  {
    namedVariable(name, false);
    expression();
    emitByte(OP_DIVIDE);
    emitBytes(setOp, (uint8_t)arg);
  }
  else if (canAssign && match(TOKEN_EQUAL))
  {
    expression();
    emitBytes(setOp, (uint8_t)arg);
  }
  else
  {
    emitBytes(getOp, (uint8_t)arg);
  }
}

static void variable(bool canAssign)
{
  namedVariable(parser.previous, canAssign);
}

static Token syntheticToken(const char *text)
{
  Token token;
  token.start = text;
  token.length = (int)strlen(text);
  return token;
}

static void beginSyntheticCall(const char *name)
{
  Token tok = syntheticToken(name);
  getVariable(tok);
}

static void endSyntheticCall(uint8_t len)
{
  emitBytes(OP_CALL, len);
}

static void syntheticCall(const char *name, Value value)
{
  beginSyntheticCall(name);
  emitConstant(value);
  endSyntheticCall(1);
}

static void pushSuperclass()
{
  if (currentClass == NULL)
    return;
  namedVariable(syntheticToken("super"), false);
}

static void super_(bool canAssign)
{
  if (currentClass == NULL)
  {
    error("Cannot use 'super' outside of a class.");
  }
  else if (!currentClass->hasSuperclass)
  {
    error("Cannot use 'super' in a class with no superclass.");
  }

  consume(TOKEN_DOT, "Expect '.' after 'super'.");
  consume(TOKEN_IDENTIFIER, "Expect superclass method name.");
  uint8_t name = identifierConstant(&parser.previous);

  namedVariable(syntheticToken("this"), false);

  if (match(TOKEN_LEFT_PAREN))
  {
    uint8_t argCount = argumentList();

    pushSuperclass();
    emitBytes(OP_SUPER, argCount);
    emitByte(name);
  }
  else
  {
    pushSuperclass();
    emitBytes(OP_GET_SUPER, name);
  }
}

static void this_(bool canAssign)
{
  if (currentClass == NULL)
  {
    error("Cannot use 'this' outside of a class.");
  }
  else if (staticMethod)
    error("Cannot use 'this' inside a static method.");
  else
  {
    variable(false);
  }
}

static void unary(bool canAssign)
{
  TokenType operatorType = parser.previous.type;

  parsePrecedence(PREC_UNARY);
  switch (operatorType)
  {
  case TOKEN_BANG:
    emitByte(OP_NOT);
    break;
  case TOKEN_MINUS:
    emitByte(OP_NEGATE);
    break;
  default:
    return; // Unreachable.
  }
}

static void block()
{
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
  {
    declaration(true);
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static uint8_t createSyntheticVariable(const char *name, Token *token)
{
  *token = syntheticToken(name);
  declareNamedVariable(token);
  uint8_t it;
  if (current->scopeDepth > 0)
    it = 0;
  else
    it = identifierConstant(token);
  return it;
}

static void function(FunctionType type)
{
  Compiler compiler;
  initCompiler(&compiler, type);
  beginScope(); // [no-end-scope]

  // Compile the parameter list.
  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
  if (!check(TOKEN_RIGHT_PAREN))
  {
    do
    {
      current->function->arity++;
      if (current->function->arity > 255)
      {
        errorAtCurrent("Cannot have more than 255 parameters.");
      }

      uint8_t paramConstant = parseVariable("Expect parameter name.");
      // if (match(TOKEN_EQUAL))
      // {
      //   expression();
      // }
      // else
      // {
      //   emitConstant(NUMBER_VAL(current->function->arity));
      // }

      defineVariable(paramConstant);
    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

  // Create args
  // Token argsToken;
  // uint8_t args = createSyntheticVariable("args", &argsToken);
  // emitByte(OP_NONE);
  // defineVariable(args);
  // Token argsInternToken = syntheticToken("__args");
  // getVariable(argsInternToken);
  // setVariablePop(argsToken);

  // The body.
  consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
  block();

  // Check this out
  endScope();

  if (type == TYPE_STATIC)
    staticMethod = false;

  // Create the function object.
  ObjFunction *function = endCompiler();
  emitBytes(OP_CLOSURE, makeConstant(OBJ_VAL(function)));

  for (int i = 0; i < function->upvalueCount; i++)
  {
    emitByte(compiler.upvalues[i].isLocal ? 1 : 0);
    emitByte(compiler.upvalues[i].index);
  }
}

static void lambda(bool canAssign)
{
  TokenType operatorType = parser.previous.type;
  markInitialized();
  function(TYPE_FUNCTION);
}

static void expand(bool canAssign)
{
  parsePrecedence(PREC_UNARY);

  if (match(TOKEN_COLON))
  {
    parsePrecedence(PREC_UNARY);
  }
  else
  {
    emitByte(OP_NONE);
  }

  emitByte(OP_EXPAND);
}

static void prefix(bool canAssign)
{
  TokenType operatorType = parser.previous.type;
  Token cur = parser.current;
  consume(TOKEN_IDENTIFIER, "Expected variable");
  namedVariable(parser.previous, true);

  int arg;
  bool instance = false;

  if (match(TOKEN_DOT))
  {
    consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
    arg = identifierConstant(&parser.previous);
    emitBytes(OP_GET_PROPERTY_NO_POP, arg);
    instance = true;
  }

  switch (operatorType)
  {
  case TOKEN_INC:
    emitByte(OP_INC);
    break;
  case TOKEN_DEC:
    emitByte(OP_DEC);
    break;
  default:
    return;
  }

  if (instance)
    emitBytes(OP_SET_PROPERTY, arg);
  else
  {
    uint8_t setOp;
    //int arg = resolveLocal(current, &cur, false);
    int arg = resolveLocal(current, &cur, false);

    if (arg != -1)
      setOp = OP_SET_LOCAL;
    else if ((arg = resolveUpvalue(current, &cur)) != -1)
      setOp = OP_SET_UPVALUE;
    else
    {
      arg = identifierConstant(&cur);
      setOp = OP_SET_GLOBAL;
    }

    emitBytes(setOp, (uint8_t)arg);
  }
}

static void let(bool canAssign)
{
  beginScope();

  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'let'.");

  while (!check(TOKEN_RIGHT_PAREN) && !check(TOKEN_EOF))
  {
    declaration(false);
    if (check(TOKEN_COMMA))
      consume(TOKEN_COMMA, "Expect ',' between 'let' expressions.");
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after 'let' expressions.");

  consume(TOKEN_LEFT_BRACE, "Expect '{' before 'let' body.");
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
  {
    declaration(true);
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after 'let' body.");
  endScope();
  emitByte(OP_NONE);
}

static void static_(bool canAssign) {
	if (currentClass == NULL)
		error("Cannot use 'static' outside of a class.");
}

ParseRule rules[] = {
    {grouping, call, PREC_CALL},     // TOKEN_LEFT_PAREN
    {NULL, NULL, PREC_NONE},         // TOKEN_RIGHT_PAREN
    {dict, NULL, PREC_NONE},         // TOKEN_LEFT_BRACE [big]
    {NULL, NULL, PREC_NONE},         // TOKEN_RIGHT_BRACE
    {list, subscript, PREC_CALL},    // TOKEN_LEFT_BRACKET
    {NULL, NULL, PREC_NONE},         // TOKEN_RIGHT_BRACKET
    {NULL, NULL, PREC_NONE},         // TOKEN_COMMA
    {NULL, dot, PREC_CALL},          // TOKEN_DOT
    {unary, binary, PREC_TERM},      // TOKEN_MINUS
    {NULL, binary, PREC_TERM},       // TOKEN_PLUS
    {prefix, NULL, PREC_NONE},       // TOKEN_INC
    {prefix, NULL, PREC_NONE},       // TOKEN_DEC
    {NULL, NULL, PREC_NONE},         // TOKEN_PLUS_EQUALS
    {NULL, NULL, PREC_NONE},         // TOKEN_MINUS_EQUALS
    {NULL, NULL, PREC_NONE},         // TOKEN_MULTIPLY_EQUALS
    {NULL, NULL, PREC_NONE},         // TOKEN_DIVIDE_EQUALS
    {NULL, NULL, PREC_NONE},         // TOKEN_SEMICOLON
    {NULL, expand, PREC_FACTOR},     // TOKEN_COLON
    {NULL, binary, PREC_FACTOR},     // TOKEN_SLASH
    {NULL, binary, PREC_FACTOR},     // TOKEN_STAR
    {NULL, binary, PREC_FACTOR},     // TOKEN_PERCENT
    {NULL, binary, PREC_FACTOR},     // TOKEN_POW
    {unary, NULL, PREC_NONE},        // TOKEN_BANG
    {NULL, binary, PREC_EQUALITY},   // TOKEN_BANG_EQUAL
    {NULL, NULL, PREC_NONE},         // TOKEN_EQUAL
    {NULL, binary, PREC_EQUALITY},   // TOKEN_EQUAL_EQUAL
    {NULL, binary, PREC_COMPARISON}, // TOKEN_GREATER
    {NULL, binary, PREC_COMPARISON}, // TOKEN_GREATER_EQUAL
    {NULL, binary, PREC_COMPARISON}, // TOKEN_LESS
    {NULL, binary, PREC_COMPARISON}, // TOKEN_LESS_EQUAL
    {variable, NULL, PREC_NONE},     // TOKEN_IDENTIFIER
    {string, NULL, PREC_NONE},       // TOKEN_STRING
    {number, NULL, PREC_NONE},       // TOKEN_NUMBER
    {byte, NULL, PREC_NONE},         // TOKEN_BYTE
    {NULL, and_, PREC_AND},          // TOKEN_AND
    {NULL, NULL, PREC_NONE},         // TOKEN_CLASS
    {static_, NULL, PREC_NONE},         // TOKEN_STATIC
    {NULL, NULL, PREC_NONE},         // TOKEN_ELSE
    {literal, NULL, PREC_NONE},      // TOKEN_FALSE
    {NULL, NULL, PREC_NONE},         // TOKEN_FOR
    {NULL, NULL, PREC_NONE},         // TOKEN_FUNC
    {lambda, NULL, PREC_NONE},       // TOKEN_LAMBDA
    {NULL, NULL, PREC_NONE},         // TOKEN_IF
    {literal, NULL, PREC_NONE},      // TOKEN_NONE
    {NULL, or_, PREC_OR},            // TOKEN_OR
    {NULL, NULL, PREC_NONE},         // TOKEN_RETURN
    {super_, NULL, PREC_NONE},       // TOKEN_SUPER
    {this_, NULL, PREC_NONE},        // TOKEN_THIS
    {literal, NULL, PREC_NONE},      // TOKEN_TRUE
    {NULL, NULL, PREC_NONE},         // TOKEN_VAR
    {NULL, NULL, PREC_NONE},         // TOKEN_WHILE
    {NULL, NULL, PREC_NONE},         // TOKEN_DO
    {NULL, binary, PREC_TERM},       // TOKEN_IN
    {NULL, is, PREC_TERM},           // TOKEN_IS
    {NULL, NULL, PREC_NONE},         // TOKEN_CONTINUE
    {NULL, NULL, PREC_NONE},         // TOKEN_BREAK
    {number, NULL, PREC_NONE},       // TOKEN_NAN
    {number, NULL, PREC_NONE},       // TOKEN_INF
    {NULL, NULL, PREC_NONE},         // TOKEN_IMPORT
    {NULL, NULL, PREC_NONE},         // TOKEN_AS
    {NULL, NULL, PREC_NONE},         // TOKEN_NAMESPACE
    {NULL, NULL, PREC_NONE},         // TOKEN_NATIVE
    {let, NULL, PREC_NONE},          // TOKEN_LET
    {NULL, NULL, PREC_NONE},         // TOKEN_WITH
    {NULL, NULL, PREC_NONE},         // TOKEN_ERROR
    {NULL, NULL, PREC_NONE},         // TOKEN_EOF
};

static void parsePrecedence(Precedence precedence)
{
  advance();
  ParseFn prefixRule = getRule(parser.previous.type)->prefix;
  if (prefixRule == NULL)
  {
    error("Expect expression.");
    return;
  }

  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(canAssign);

  while (precedence <= getRule(parser.current.type)->precedence)
  {
    advance();
    ParseFn infixRule = getRule(parser.previous.type)->infix;
    infixRule(canAssign);
  }

  if (canAssign && match(TOKEN_EQUAL))
  {
    error("Invalid assignment target.");
  }
}

static ParseRule *getRule(TokenType type)
{
  return &rules[type];
}

static void expression()
{
  parsePrecedence(PREC_ASSIGNMENT);
}

static void method(bool isStatic)
{
  FunctionType type;

  if (isStatic)
  {
    type = TYPE_STATIC;
    staticMethod = true;
  }
  else
    type = TYPE_METHOD;

  consume(TOKEN_FUNC, "Expect a function declaration.");
  if (!check(TOKEN_IDENTIFIER) && !isOperator(parser.current.type))
  {
    errorAtCurrent("Expect method name.");
  }
  advance();

  uint8_t constant = identifierConstant(&parser.previous);

  // If the method is named "init", it's an initializer.
  if (parser.previous.length == 4 &&
      memcmp(parser.previous.start, "init", 4) == 0)
  {
    type = TYPE_INITIALIZER;
  }

  function(type);

  emitBytes(OP_METHOD, constant);
}

static void property(bool isStatic)
{
  uint8_t name = parseVariable("Expect variable name.");

  if (match(TOKEN_EQUAL))
  {
    expression();
  }
  else
  {
    emitByte(OP_NONE);
  }
  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
  
  emitByte(isStatic ? OP_TRUE : OP_FALSE);
  emitBytes(OP_PROPERTY, name);
}

static void methodOrProperty()
{
  bool isStatic = false;
  if (match(TOKEN_STATIC))
  {
    isStatic = true;
  }

  if (check(TOKEN_FUNC))
  {
    method(isStatic);
  }
  else if (check(TOKEN_VAR))
  {
    consume(TOKEN_VAR, "Expected a variable declaration.");
    property(isStatic);
  }
  else
  {
    errorAtCurrent("Only variables and functions allowed inside a class.");
  }
}

static void funcOrField()
{
  if (check(TOKEN_FUNC))
  {
    method(false);
  }
  else if (check(TOKEN_VAR))
  {
    consume(TOKEN_VAR, "Expected a variable declaration.");
    property(false);
  }
  else
  {
    errorAtCurrent("Only variables and functions allowed inside a class.");
  }
}

static void nativeFunc()
{
  consume(TOKEN_IDENTIFIER, "Expect type name.");
  int len = parser.previous.length + 1;
  char *str = malloc(sizeof(char) * len);
  strncpy(str, parser.previous.start, parser.previous.length);
  str[parser.previous.length] = '\0';
  emitConstant(OBJ_VAL(copyString(str, strlen(str))));
  free(str);

  consume(TOKEN_IDENTIFIER, "Expect function name.");
  Token name = parser.previous;
  uint8_t nameConstant = identifierConstant(&parser.previous);
  declareVariable();

  //uint8_t name = parseVariable("Expect funcion name.");
  //Token tokName = parser.previous;

  // consume(TOKEN_IDENTIFIER, "Expect function name.");
  len = parser.previous.length + 1;
  str = malloc(sizeof(char) * len);
  strncpy(str, parser.previous.start, parser.previous.length);
  str[parser.previous.length] = '\0';
  emitConstant(OBJ_VAL(copyString(str, strlen(str))));
  free(str);

  // Compile the parameter list.
  consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");
  int arity = 0;

  if (!check(TOKEN_RIGHT_PAREN))
  {
    do
    {
      arity++;
      if (arity > 10)
      {
        errorAtCurrent("Cannot have more than 10 parameters.");
      }

      consume(TOKEN_IDENTIFIER, "Expect parameter type.");
      len = parser.previous.length + 1;
      str = malloc(sizeof(char) * len);
      strncpy(str, parser.previous.start, parser.previous.length);
      str[parser.previous.length] = '\0';
      emitConstant(OBJ_VAL(copyString(str, strlen(str))));
      free(str);

    } while (match(TOKEN_COMMA));
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");
  consume(TOKEN_SEMICOLON, "Expect ';' after native func declaration.");

  emitConstant(NUMBER_VAL(arity));

  emitByte(OP_NONE);
  defineVariable(nameConstant);

  emitByte(OP_NATIVE_FUNC);

  setVariablePop(name);
}

static void namespaceDeclaration()
{
  consume(TOKEN_IDENTIFIER, "Expect namespace.");
  Token namespace = parser.previous;
  uint8_t nameConstant = identifierConstant(&parser.previous);
  declareVariable();

  emitBytes(OP_NAMESPACE, nameConstant);
  defineVariable(nameConstant);

  NamespaceCompiler namespaceCompiler;
  namespaceCompiler.name = parser.previous;
  namespaceCompiler.enclosing = currentNamespace;
  currentNamespace = &namespaceCompiler;

  //beginScope();

  consume(TOKEN_LEFT_BRACE, "Expect '{' before namespace body.");
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
  {
    namedVariable(namespace, false);
    funcOrField();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after namespace body.");

  //endScope();

  currentNamespace = currentNamespace->enclosing;
}

static void classDeclaration()
{
  consume(TOKEN_IDENTIFIER, "Expect class name.");
  Token className = parser.previous;
  uint8_t nameConstant = identifierConstant(&parser.previous);
  declareVariable();

  emitBytes(OP_CLASS, nameConstant);
  defineVariable(nameConstant);

  ClassCompiler classCompiler;
  classCompiler.name = parser.previous;
  classCompiler.hasSuperclass = false;
  classCompiler.enclosing = currentClass;
  currentClass = &classCompiler;

  if (match(TOKEN_COLON))
  {
    consume(TOKEN_IDENTIFIER, "Expect superclass name.");

    if (identifiersEqual(&className, &parser.previous))
    {
      error("A class cannot inherit from itself.");
    }

    classCompiler.hasSuperclass = true;

    beginScope();

    // Store the superclass in a local variable named "super".
    variable(false);
    addLocal(syntheticToken("super"));
    defineVariable(0);

    namedVariable(className, false);
    emitByte(OP_INHERIT);
  }

  consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
  {
    namedVariable(className, false);
    methodOrProperty();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");

  if (classCompiler.hasSuperclass)
  {
    endScope();
  }

  currentClass = currentClass->enclosing;
}

static void pathOrString(const char *extension, const char *errorIdentifier, const char *errorString)
{
  if (match(TOKEN_IDENTIFIER))
  {
    int len = parser.previous.length;
    int totalLen = len;
    char *str = ALLOCATE(char, len + 1);
    strncpy(str, parser.previous.start, parser.previous.length);
    str[len] = '\0';

    while (true)
    {
      if (match(TOKEN_SLASH))
      {
        if (match(TOKEN_IDENTIFIER))
        {
          totalLen += parser.previous.length + 1;
          str = GROW_ARRAY(str, char, len, totalLen);
          str[len] = '/';
          strncpy(str + (len + 1), parser.previous.start, parser.previous.length);
          len = totalLen;
          str[len] = '\0';
        }
        else
        {
          errorAtCurrent(errorIdentifier);
        }
      }
      else
      {
        break;
      }
    }

    totalLen = len + strlen(extension);
    str = GROW_ARRAY(str, char, len, totalLen);
    len = totalLen;
    strcat(str, extension);

    emitConstant(OBJ_VAL(copyString(str, strlen(str))));
    FREE_ARRAY(char, str, totalLen);
  }
  else
  {
    consume(TOKEN_STRING, errorString);
    emitConstant(OBJ_VAL(
        copyString(parser.previous.start + 1, parser.previous.length - 2)));
  }
}

static void nativeDeclaration()
{
  pathOrString(vm.nativeExtension, "Expect an identifier after slash in native.", "Expect string after native.");

  int count = 0;
  consume(TOKEN_LEFT_BRACE, "Expect '{' before native body.");
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF))
  {
    nativeFunc();
    count++;
  }
  consume(TOKEN_RIGHT_BRACE, "Expect '}' after native body.");

  emitConstant(NUMBER_VAL(count));
  emitByte(OP_NATIVE);
}

static void funDeclaration()
{
  uint8_t global = parseVariable("Expect function name.");
  markInitialized();
  function(TYPE_FUNCTION);
  defineVariable(global);
}

static void varDeclaration(bool checkEnd)
{
  uint8_t global = parseVariable("Expect variable name.");

  if (match(TOKEN_EQUAL))
  {
    expression();
  }
  else
  {
    emitByte(OP_NONE);
  }
  if (checkEnd)
    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  defineVariable(global);
}

static void expressionStatement()
{
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}

static void forStatement()
{
  beginScope();
  current->loopDepth++;

  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");

  bool in = false;
  uint8_t name;
  uint8_t it, var, cond;
  Token nameVar, loopVar, valVar, condVar;
  int exitJump = -1;
  int surroundingLoopStart;
  int surroundingLoopScopeDepth;

  if (match(TOKEN_SEMICOLON))
  {
    // No initializer.
  }
  else if (match(TOKEN_VAR))
  {
    name = parseVariable("Expect variable name.");
    nameVar = parser.previous;
    if (match(TOKEN_EQUAL))
    {
      expression();
    }
    else
    {
      if (match(TOKEN_IN))
      {
        emitByte(OP_NONE);
        defineVariable(name);

        var = createSyntheticVariable("__value", &valVar);
        emitByte(OP_NONE);
        defineVariable(var);

        expression();
        setVariablePop(valVar);

        it = createSyntheticVariable("__index", &loopVar);
        emitByte(OP_NONE);
        defineVariable(it);

        emitConstant(NUMBER_VAL(0));
        setVariablePop(loopVar);

        cond = createSyntheticVariable("__cond", &condVar);
        emitByte(OP_NONE);
        defineVariable(cond);

        emitConstant(NUMBER_VAL(0));
        setVariablePop(condVar);

        /*
        getVariable(valVar);
        getVariable(loopVar);
        emitByte(OP_SUBSCRIPT);
        setVariablePop(nameVar);
        */

        in = true;
      }
      else
        emitByte(OP_NONE);
    }

    if (!in)
    {
      consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
      defineVariable(name);
    }
  }
  else
  {
    expressionStatement();
  }

  surroundingLoopStart = innermostLoopStart;
  surroundingLoopScopeDepth = innermostLoopScopeDepth;
  innermostLoopStart = currentChunk()->count;
  innermostLoopScopeDepth = current->scopeDepth;

  if (!in && !match(TOKEN_SEMICOLON))
  {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Condition.
  }
  else if (in)
  {
    beginSyntheticCall("len");
    getVariable(valVar);
    endSyntheticCall(1);
    setVariablePop(condVar);

    getVariable(loopVar);
    getVariable(condVar);
    //emitConstant(NUMBER_VAL(5));
    emitByte(OP_LESS);
    exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP); // Condition.
  }

  if (!in && !match(TOKEN_RIGHT_PAREN))
  {
    int bodyJump = emitJump(OP_JUMP);

    int incrementStart = currentChunk()->count;
    expression();
    emitByte(OP_POP);
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(innermostLoopStart);
    innermostLoopStart = incrementStart;
    patchJump(bodyJump);
  }
  else if (in)
  {
    int bodyJump = emitJump(OP_JUMP);
    int incrementStart = currentChunk()->count;

    // Increment counter
    getVariable(loopVar);
    emitConstant(NUMBER_VAL(1));
    emitByte(OP_ADD);
    setVariablePop(loopVar);
    emitByte(OP_POP);

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");

    emitLoop(innermostLoopStart);
    innermostLoopStart = incrementStart;
    patchJump(bodyJump);

    // Set variable
    getVariable(valVar);
    getVariable(loopVar);
    emitByte(OP_SUBSCRIPT);
    setVariablePop(nameVar);
  }

  statement();

  // Jump back to the beginning (or the increment).
  emitLoop(innermostLoopStart);

  if (exitJump != -1)
  {
    patchJump(exitJump);
    emitByte(OP_POP); // Condition.
  }

  if (in)
  {
    setVariable(loopVar, NUMBER_VAL(0));
    setVariable(condVar, NUMBER_VAL(0));
    setVariable(valVar, NUMBER_VAL(0));
  }

  innermostLoopStart = surroundingLoopStart;
  innermostLoopScopeDepth = surroundingLoopScopeDepth;

  endScope();
  current->loopDepth--;
}

static void ifStatement()
{
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition."); // [paren]

  int thenJump = emitJump(OP_JUMP_IF_FALSE);
  emitByte(OP_POP);
  statement();

  int elseJump = emitJump(OP_JUMP);

  patchJump(thenJump);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE))
    statement();
  patchJump(elseJump);
}

static void withStatement()
{
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'with'.");
  expression();
  if (match(TOKEN_COMMA))
  {
    expression();
  }
  else
  {
    emitConstant(STRING_VAL("r"));
  }

  consume(TOKEN_RIGHT_PAREN, "Expect ')' after 'with'.");

  beginScope();

  Token file = syntheticToken("file");
  Local *local = &current->locals[current->localCount++];
  local->depth = current->scopeDepth;
  local->isCaptured = false;
  local->name = file;

  emitByte(OP_FILE);

  statement();

  getVariable(file);
  emitBytes(OP_INVOKE, 0);

  Token fn = syntheticToken("close");
  uint8_t name = identifierConstant(&fn);
  emitByte(name);

  endScope();
}

static void returnStatement()
{
  if (current->type == TYPE_SCRIPT)
  {
    error("Cannot return from top-level code.");
  }

  if (match(TOKEN_SEMICOLON))
  {
    emitReturn();
  }
  else
  {
    if (current->type == TYPE_INITIALIZER)
    {
      error("Cannot return a value from an initializer.");
    }

    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
    emitByte(OP_RETURN);
  }
}

static void importStatement()
{
  pathOrString(vm.extension, "Expect an identifier after slash in import.", "Expect string after import.");

  if (match(TOKEN_AS))
  {
    if (match(TOKEN_IDENTIFIER))
    {
      int len = parser.previous.length + 1;
      char *str = malloc(sizeof(char) * len);
      strncpy(str, parser.previous.start, parser.previous.length);
      str[parser.previous.length] = '\0';
      emitConstant(OBJ_VAL(copyString(str, strlen(str))));
      free(str);
    }
    else
    {
      consume(TOKEN_STRING, "Expect string after as.");

      emitConstant(OBJ_VAL(
          copyString(parser.previous.start + 1, parser.previous.length - 2)));
    }
  }
  else
  {
    emitByte(OP_NONE);
  }

  consume(TOKEN_SEMICOLON, "Expect ';' after import.");

  emitByte(OP_IMPORT);
}

static void whileStatement()
{
  current->loopDepth++;

  int surroundingLoopStart = innermostLoopStart;
  int surroundingLoopScopeDepth = innermostLoopScopeDepth;
  innermostLoopStart = currentChunk()->count;
  innermostLoopScopeDepth = current->scopeDepth;

  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP);
  statement();

  // Loop back to the start.
  emitLoop(innermostLoopStart);

  patchJump(exitJump);
  emitByte(OP_POP);

  innermostLoopStart = surroundingLoopStart;
  innermostLoopScopeDepth = surroundingLoopScopeDepth;

  current->loopDepth--;
}

static void doWhileStatement()
{
  current->loopDepth++;

  int surroundingLoopStart = innermostLoopStart;
  int surroundingLoopScopeDepth = innermostLoopScopeDepth;
  innermostLoopStart = currentChunk()->count;
  innermostLoopScopeDepth = current->scopeDepth;

  statement();

  consume(TOKEN_WHILE, "Expected 'while' after 'do' body.");
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");
  consume(TOKEN_SEMICOLON, "Expected semicolon after 'do while'");

  int exitJump = emitJump(OP_JUMP_IF_FALSE);

  emitByte(OP_POP);

  // Loop back to the start.
  emitLoop(innermostLoopStart);

  patchJump(exitJump);
  emitByte(OP_POP);

  innermostLoopStart = surroundingLoopStart;
  innermostLoopScopeDepth = surroundingLoopScopeDepth;

  current->loopDepth--;
}

static void continueStatement()
{
  if (innermostLoopStart == -1)
    error("Cannot use 'continue' outside of a loop.");

  consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");

  // Discard any locals created inside the loop.
  for (int i = current->localCount - 1;
       i >= 0 && current->locals[i].depth > innermostLoopScopeDepth; i--)
    emitByte(OP_POP);

  // Jump to top of current innermost loop.
  emitLoop(innermostLoopStart);
}

static void breakStatement()
{
  if (current->loopDepth == 0)
  {
    error("Cannot use 'break' outside of a loop.");
    return;
  }

  consume(TOKEN_SEMICOLON, "Expected semicolon after break");
  emitByte(OP_BREAK);
}

static void synchronize()
{
  parser.panicMode = false;

  while (parser.current.type != TOKEN_EOF)
  {
    if (parser.previous.type == TOKEN_SEMICOLON)
      return;

    switch (parser.current.type)
    {
    case TOKEN_CLASS:
    case TOKEN_NAMESPACE:
    case TOKEN_NATIVE:
    case TOKEN_FUNC:
    case TOKEN_STATIC:
    case TOKEN_VAR:
    case TOKEN_FOR:
    case TOKEN_IF:
    case TOKEN_WHILE:
    case TOKEN_RETURN:
    case TOKEN_IMPORT:
    case TOKEN_BREAK:
    case TOKEN_WITH:
      return;

    default:
        // Do nothing.
        ;
    }

    advance();
  }
}

static void declaration(bool checkEnd)
{
  if (match(TOKEN_CLASS))
  {
    classDeclaration();
  }
  else if (match(TOKEN_NAMESPACE))
  {
    namespaceDeclaration();
  }
  else if (match(TOKEN_NATIVE))
  {
    nativeDeclaration();
  }
  else if (match(TOKEN_FUNC))
  {
    funDeclaration();
  }
  else if (match(TOKEN_VAR))
  {
    varDeclaration(checkEnd);
  }
  else
  {
    statement();
  }

  if (parser.panicMode)
    synchronize();
}

static void statement()
{
  if (match(TOKEN_FOR))
  {
    forStatement();
  }
  else if (match(TOKEN_IF))
  {
    ifStatement();
  }
  else if (match(TOKEN_RETURN))
  {
    returnStatement();
  }
  else if (match(TOKEN_WITH))
  {
    withStatement();
  }
  else if (match(TOKEN_IMPORT))
  {
    importStatement();
  }
  else if (match(TOKEN_BREAK))
  {
    breakStatement();
  }
  else if (match(TOKEN_CONTINUE))
  {
    continueStatement();
  }
  else if (match(TOKEN_WHILE))
  {
    whileStatement();
  }
  else if (match(TOKEN_DO))
  {
    doWhileStatement();
  }
  else if (match(TOKEN_LEFT_BRACE))
  {
    Token previous = parser.previous;
    Token current = parser.current;
    if (check(TOKEN_STRING))
    {
      for (int i = 0;
           i < parser.current.length - parser.previous.length + 1; ++i)
        backTrack();

      parser.current = previous;
      expressionStatement();
      return;
    }
    else if (check(TOKEN_RIGHT_BRACE))
    {
      advance();
      if (check(TOKEN_SEMICOLON))
      {
        backTrack();
        backTrack();
        parser.current = previous;
        expressionStatement();
        return;
      }
    }
    parser.current = current;

    beginScope();
    block();
    endScope();
  }
  else
  {
    expressionStatement();
  }
}

ObjFunction *compile(const char *source)
{
  ObjFunction *function = NULL;
  if (memcmp(initString, source, strlen(initString)) == 0)
  {
    char *src = (char *)source + strlen(initString);

    uint32_t len = ((uint32_t *)src)[0];
    src += sizeof(uint32_t);

    uint32_t pos = 0;
    Value value = loadByteCode(src, &pos, len);
    if (IS_FUNCTION(value))
      function = AS_FUNCTION(value);
  }
  else
  {
    initScanner(source);
    Compiler compiler;

    initCompiler(&compiler, TYPE_SCRIPT);

    parser.hadError = false;
    parser.panicMode = false;

    // Create args
    // Token argsToken = syntheticToken("args");
    // uint8_t args = identifierConstant(&argsToken);
    // getVariable(syntheticToken("__args"));
    // defineVariable(args);

    // Parse the code
    advance();

    while (!match(TOKEN_EOF))
    {
      declaration(true);
    }

    function = endCompiler();
    if (parser.hadError)
      function = NULL;
  }
  return function;
}

void grayCompilerRoots()
{
  Compiler *compiler = current;
  while (compiler != NULL)
  {
    grayObject((Obj *)compiler->function);
    compiler = compiler->enclosing;
  }
}

bool writeByteCodeChunk(FILE *file, Chunk *chunk);

bool initByteCode(FILE *file)
{
  uint32_t sz = 0;
  if (fwrite(initString, sizeof(char), strlen(initString), file) != strlen(initString))
    return false;
  if (fwrite(&sz, sizeof(sz), 1, file) != 1)
    return false;
  return true;
}

bool writeByteCode(FILE *file, Value value)
{
  uint32_t type = value.type;
  uint32_t objType = 0;

  if (IS_OBJ(value))
    objType = OBJ_TYPE(value);

  // char *typeName = valueType(value);
  // printf("Write: <%s> ", typeName);
  // printValue(value);
  // printf("\n");
  // free(typeName);

  int i = 0;
  if (fwrite(&type, sizeof(type), 1, file) != 1)
    return false;
  if (fwrite(&objType, sizeof(objType), 1, file) != 1)
    return false;

  if (IS_BOOL(value))
  {
    if (fwrite(&value.as.boolean, sizeof(value.as.boolean), 1, file) != 1)
      return false;
  }
  else if (IS_NUMBER(value))
  {
    if (fwrite(&value.as.number, sizeof(value.as.number), 1, file) != 1)
      return false;
  }
  else if (IS_STRING(value))
  {
    ObjString *str = AS_STRING(value);
    if (fwrite(&str->length, sizeof(str->length), 1, file) != 1)
      return false;
    if (fwrite(str->chars, sizeof(char), str->length, file) != str->length)
      return false;
  }
  else if (IS_BYTES(value))
  {
    ObjBytes *bytes = AS_BYTES(value);
    if (fwrite(&bytes->length, sizeof(bytes->length), 1, file) != 1)
      return false;
    if (fwrite(bytes->bytes, sizeof(unsigned char), bytes->length, file) != bytes->length)
      return false;
  }
  else if (IS_LIST(value))
  {
    ObjList *list = AS_LIST(value);
    if (fwrite(&list->values.count, sizeof(list->values.count), 1, file) != 1)
      return false;
    for (i = 0; i < list->values.count; i++)
    {
      if (!writeByteCode(file, list->values.values[i]))
        return false;
    }
  }
  else if (IS_DICT(value))
  {
    ObjDict *dict = AS_DICT(value);
    if (fwrite(&dict->count, sizeof(dict->count), 1, file) != 1)
      return false;
    for (i = 0; i < dict->capacity; i++)
    {
      if (!dict->items[i])
        continue;

      Value key = STRING_VAL(dict->items[i]->key);

      if (!writeByteCode(file, key))
        return false;
      if (!writeByteCode(file, dict->items[i]->item))
        return false;
    }
  }
  else if (IS_NATIVE_FUNC(value))
  {
    printf("Invalid bytecode: NativeFunc (Valid only on runtime)\n");
    return false;
  }
  else if (IS_NATIVE_LIB(value))
  {
    printf("Invalid bytecode: NativeLib (Valid only on runtime)\n");
    return false;
  }
  else if (IS_NATIVE(value))
  {
    printf("Invalid bytecode: Native (Valid only on runtime)\n");
    return false;
  }
  else if (IS_INSTANCE(value))
  {
    printf("Invalid bytecode: Instance (Valid only on runtime)\n");
    return false;
  }
  else if (IS_FUNCTION(value))
  {
    ObjFunction *func = AS_FUNCTION(value);
    if (func->name == NULL)
    {
      Value name = STRING_VAL(vm.scriptName);
      if (!writeByteCode(file, name))
        return false;
    }
    else
    {
      if (!writeByteCode(file, OBJ_VAL(func->name)))
        return false;
    }
    if (fwrite(&func->arity, sizeof(func->arity), 1, file) != 1)
      return false;
    if (fwrite(&func->staticMethod, sizeof(func->staticMethod), 1, file) != 1)
      return false;
    if (fwrite(&func->upvalueCount, sizeof(func->upvalueCount), 1, file) != 1)
      return false;
    if (!writeByteCodeChunk(file, &func->chunk))
      return false;
  }
  else if (IS_CLOSURE(value))
  {
    ObjClosure *closure = AS_CLOSURE(value);
    if (!writeByteCode(file, OBJ_VAL(closure->function)))
      return false;
    if (fwrite(&closure->upvalueCount, sizeof(closure->upvalueCount), 1, file) != 1)
      return false;
    for (i = 0; i < closure->upvalueCount; i++)
    {
      if (!writeByteCode(file, OBJ_VAL(closure->upvalues[i])))
        return false;
    }
  }
  else if (IS_BOUND_METHOD(value))
  {
    printf("Invalid bytecode: Bound Method (Valid only on runtime)\n");
    return false;
  }
  else if (IS_CLASS(value))
  {
    ObjClass *klass = AS_CLASS(value);
    if (!writeByteCode(file, OBJ_VAL(klass->name)))
      return false;

    if (fwrite(&klass->fields.count, sizeof(klass->fields.count), 1, file) != 1)
      return false;
    i = 0;
    Entry entry;
    while (iterateTable(&klass->fields, &entry, &i))
    {
      if (entry.key == NULL)
        continue;

      if (!writeByteCode(file, OBJ_VAL(entry.key)))
        return false;

      if (!writeByteCode(file, entry.value))
        return false;
    }

    if (fwrite(&klass->methods.count, sizeof(klass->methods.count), 1, file) != 1)
      return false;
    i = 0;
    while (iterateTable(&klass->methods, &entry, &i))
    {
      if (entry.key == NULL)
        continue;

      if (!writeByteCode(file, OBJ_VAL(entry.key)))
        return false;

      if (!writeByteCode(file, entry.value))
        return false;
    }

    if (fwrite(&klass->staticFields.count, sizeof(klass->staticFields.count), 1, file) != 1)
      return false;
    i = 0;
    while (iterateTable(&klass->staticFields, &entry, &i))
    {
      if (entry.key == NULL)
        continue;

      if (!writeByteCode(file, OBJ_VAL(entry.key)))
        return false;

      if (!writeByteCode(file, entry.value))
        return false;
    }
  }
  else if (IS_NAMESPACE(value))
  {
    ObjNamespace *namespace = AS_NAMESPACE(value);
    if (!writeByteCode(file, OBJ_VAL(namespace->name)))
      return false;

    if (fwrite(&namespace->fields.count, sizeof(namespace->fields.count), 1, file) != 1)
      return false;
    i = 0;
    Entry entry;
    while (iterateTable(&namespace->fields, &entry, &i))
    {
      if (entry.key == NULL)
        continue;

      if (!writeByteCode(file, OBJ_VAL(entry.key)))
        return false;

      if (!writeByteCode(file, entry.value))
        return false;
    }

    if (fwrite(&namespace->methods.count, sizeof(namespace->methods.count), 1, file) != 1)
      return false;
    i = 0;
    while (iterateTable(&namespace->methods, &entry, &i))
    {
      if (entry.key == NULL)
        continue;

      if (!writeByteCode(file, OBJ_VAL(entry.key)))
        return false;

      if (!writeByteCode(file, entry.value))
        return false;
    }
  }
  else if (value.type == OBJ_UPVALUE)
  {
    /*
    ObjUpvalue *upvalue = (ObjUpvalue *)AS_OBJ(value);
    char valid;
    if (upvalue->location)
    {
      valid = 255;
      if (fwrite(&valid, sizeof(valid), 1, file) != 1)
        return false;
      if (!writeByteCode(file, *upvalue->location))
        return false;
    }
    else
    {
      valid = 250;
      if (fwrite(&valid, sizeof(valid), 1, file) != 1)
        return false;
    }

    if (!writeByteCode(file, upvalue->closed))
      return false;

    if (upvalue->next)
    {
      valid = 255;
      if (fwrite(&valid, sizeof(valid), 1, file) != 1)
        return false;
      if (!writeByteCode(file, OBJ_VAL(upvalue->next)))
        return false;
    }
    else
    {
      valid = 250;
      if (fwrite(&valid, sizeof(valid), 1, file) != 1)
        return false;
    }
    */
    printf("Invalid bytecode: UpValue (Valid only on runtime)\n");
    return false;
  }
  return true;
}

bool writeByteCodeChunk(FILE *file, Chunk *chunk)
{
  int i = 0;
  if (fwrite(&chunk->count, sizeof(chunk->count), 1, file) != 1)
    return false;
  if (fwrite(chunk->code, sizeof(uint8_t), chunk->count, file) != chunk->count)
    return false;
  if (fwrite(chunk->lines, sizeof(int), chunk->count, file) != chunk->count)
    return false;
  if (fwrite(&chunk->constants.count, sizeof(chunk->constants.count), 1, file) != 1)
    return false;
  for (i = 0; i < chunk->constants.count; i++)
  {
    if (!writeByteCode(file, chunk->constants.values[i]))
      return false;
  }
  return true;
}

bool finishByteCode(FILE *file)
{
  fseek(file, 0L, SEEK_END);
  uint32_t fileSize = ftell(file);
  rewind(file);

  uint32_t pos = strlen(initString);

  fseek(file, pos, SEEK_SET);
  if (fwrite(&fileSize, sizeof(fileSize), 1, file) != 1)
    return false;
  return true;
}

#define READ(type)              \
  ((type *)(source + *pos))[0]; \
  *pos += sizeof(type)
#define READ_ARRAY(type, dest, len)                \
  memcpy(dest, source + *pos, len * sizeof(type)); \
  *pos += sizeof(type) * len

void loadChunk(Chunk *chunk, const char *source, uint32_t *pos, uint32_t total);

Value loadByteCode(const char *source, uint32_t *pos, uint32_t total)
{
  uint32_t type = READ(uint32_t);
  uint32_t objType = READ(uint32_t);
  // printf("Load: %d, %d\n", type, objType);

  Value value = NONE_VAL;

  int i = 0;

  if (type == VAL_NONE)
  {
    // None
  }
  else if (type == VAL_BOOL)
  {
    BOOL_TYPE v = READ(BOOL_TYPE);
    value = BOOL_VAL(v);
  }
  else if (type == VAL_NUMBER)
  {
    NUMBER_TYPE v = READ(NUMBER_TYPE);
    value = NUMBER_VAL(v);
  }
  else if (type == VAL_OBJ)
  {
    if (objType == OBJ_STRING)
    {
      int len = READ(int);
      char *str = ALLOCATE(char, len + 1);
      str[len] = '\0';
      READ_ARRAY(char, str, len);
      value = STRING_VAL(str);
      FREE_ARRAY(char, str, len + 1);
    }
    else if (objType == OBJ_BYTES)
    {
      int len = READ(int);
      unsigned char *bytes = ALLOCATE(unsigned char, len);
      READ_ARRAY(unsigned char, bytes, len);
      value = BYTES_VAL(bytes, len);
      FREE_ARRAY(unsigned char, bytes, len);
    }
    else if (objType == OBJ_LIST)
    {
      ObjList *list = initList();
      int len = READ(int);
      for (i = 0; i < len; i++)
      {
        Value obj = loadByteCode(source, pos, total);
        writeValueArray(&list->values, obj);
      }
      value = OBJ_VAL(list);
    }
    else if (objType == OBJ_DICT)
    {
      ObjDict *dict = initDict();
      int len = READ(int);
      for (i = 0; i < len; i++)
      {
        if (!dict->items[i])
          continue;

        Value key = loadByteCode(source, pos, total);
        Value val = loadByteCode(source, pos, total);
        insertDict(dict, AS_CSTRING(key), val);
      }
      value = OBJ_VAL(list);
    }
    else if (objType == OBJ_NATIVE_FUNC)
    {
      printf("Invalid bytecode: Native (Valid only on runtime)\n");
    }
    else if (objType == OBJ_NATIVE_LIB)
    {
      printf("Invalid bytecode: Native (Valid only on runtime)\n");
    }
    else if (objType == OBJ_NATIVE)
    {
      printf("Invalid bytecode: Native (Valid only on runtime)\n");
    }
    else if (objType == OBJ_INSTANCE)
    {
      printf("Invalid bytecode: Instance (Valid only on runtime)\n");
    }
    else if (objType == OBJ_FUNCTION)
    {
      ObjFunction *func = newFunction(false);
      Value name = loadByteCode(source, pos, total);
      func->name = AS_STRING(name);
      func->arity = READ(int);
      func->staticMethod = READ(bool);
      func->upvalueCount = READ(int);
      loadChunk(&func->chunk, source, pos, total);
      value = OBJ_VAL(func);
    }
    else if (objType == OBJ_CLOSURE)
    {
      Value func = loadByteCode(source, pos, total);
      ObjClosure *closure = newClosure(AS_FUNCTION(func));
      closure->upvalueCount = READ(int);
      for (i = 0; i < closure->upvalueCount; i++)
      {
        Value val = loadByteCode(source, pos, total);
        closure->upvalues[i] = (ObjUpvalue *)AS_OBJ(val);
      }
    }
    else if (objType == OBJ_BOUND_METHOD)
    {
      printf("Invalid bytecode: Bound Method (Valid only on runtime)\n");
    }
    else if (objType == OBJ_CLASS)
    {
      Value name = loadByteCode(source, pos, total);
      ObjClass *klass = newClass(AS_STRING(name));

      int len = READ(int);
      for (i = 0; i < len; i++)
      {
        Value key = loadByteCode(source, pos, total);
        Value val = loadByteCode(source, pos, total);
        tableSet(&klass->fields, AS_STRING(key), val);
      }

      len = READ(int);
      for (i = 0; i < len; i++)
      {
        Value key = loadByteCode(source, pos, total);
        Value val = loadByteCode(source, pos, total);
        tableSet(&klass->methods, AS_STRING(key), val);
      }

      len = READ(int);
      for (i = 0; i < len; i++)
      {
        Value key = loadByteCode(source, pos, total);
        Value val = loadByteCode(source, pos, total);
        tableSet(&klass->staticFields, AS_STRING(key), val);
      }
    }
    else if (objType == OBJ_NAMESPACE)
    {
      Value name = loadByteCode(source, pos, total);
      ObjNamespace *namespace = newNamespace(AS_STRING(name));

      int len = READ(int);
      for (i = 0; i < len; i++)
      {
        Value key = loadByteCode(source, pos, total);
        Value val = loadByteCode(source, pos, total);
        tableSet(&namespace->fields, AS_STRING(key), val);
      }

      len = READ(int);
      for (i = 0; i < len; i++)
      {
        Value key = loadByteCode(source, pos, total);
        Value val = loadByteCode(source, pos, total);
        tableSet(&namespace->methods, AS_STRING(key), val);
      }
    }
    else if (objType == OBJ_UPVALUE)
    {
      /*
      ObjUpvalue *upvalue = newUpvalue();
      char valid;
      if (upvalue->location)
      {
        valid = 255;
        if (fwrite(&valid, sizeof(valid), 1, file) != 1)
          return false;
        if (!writeByteCode(file, *upvalue->location))
          return false;
      }
      else
      {
        valid = 250;
        if (fwrite(&valid, sizeof(valid), 1, file) != 1)
          return false;
      }

      if (!writeByteCode(file, upvalue->closed))
        return false;

      if (upvalue->next)
      {
        valid = 255;
        if (fwrite(&valid, sizeof(valid), 1, file) != 1)
          return false;
        if (!writeByteCode(file, OBJ_VAL(upvalue->next)))
          return false;
      }
      else
      {
        valid = 250;
        if (fwrite(&valid, sizeof(valid), 1, file) != 1)
          return false;
      }
      */
      printf("Invalid bytecode: UpValue (Valid only on runtime)\n");
    }
  }
  // printf("Value: ");
  // printValue(value);
  // printf("\n");
  return value;
}

void loadChunk(Chunk *chunk, const char *source, uint32_t *pos, uint32_t total)
{
  chunk->count = READ(int);
  chunk->capacity = chunk->count;
  chunk->code = GROW_ARRAY(chunk->code, uint8_t,
                           0, chunk->capacity);
  chunk->lines = GROW_ARRAY(chunk->lines, int,
                            0, chunk->capacity);

  READ_ARRAY(uint8_t, chunk->code, chunk->count);
  READ_ARRAY(int, chunk->lines, chunk->count);

  int i = 0;
  int len = READ(int);
  for (i = 0; i < len; i++)
  {
    Value value = loadByteCode(source, pos, total);
    writeValueArray(&chunk->constants, value);
  }
}