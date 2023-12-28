#include "astparse.h"

#include "ast.h"

#include <stdio.h>

#include "../common.h"
#include "../scanner.h"
#include "../object.h"
#include "../memory.h"
#include "../types.h"

#ifdef DEBUG_PRINT_CODE

#include "../debug.h"

#endif

#include <stdlib.h>
#include <string.h>

bool beginBody = false;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =
    PREC_YIELD,       // yield
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_EQUALITY,    // == !=
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * /
    PREC_UNARY,       // ! -
    PREC_CALL,        // . ()
    PREC_PRIMARY
} Precedence;

typedef Expr *(*ParseFn)(bool canAssign);

typedef Expr *(*InfixParseFn)(Expr *left, bool canAssign);

typedef struct {
    ParseFn prefix;
    InfixParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct ClassCompiler {
    struct ClassCompiler *enclosing;
    bool hasSuperclass;
} ClassCompiler;

Parser parser;

Node *allocateNode(size_t size, NodeType type) {
    Node *node = (Node *) reallocate(NULL, 0, size);
    node->type = type;
    node->isMarked = false;
    node->next = parser.nodes;
    parser.nodes = node;

#ifdef DEBUG_LOG_GC
    printf("%p allocate %zu for node %d\n", (void *) node, size, type);
#endif

    return node;
}

static void errorAt(Token *token, const char *message) {
    if (parser.panicMode) return;
    parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // Nothing.
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char *message) {
    errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char *message) {
    errorAt(&parser.current, message);
}

static void advance() {
    parser.previous = parser.current;

    for (;;) {
        parser.current = scanToken();
        if (parser.current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser.current.start);
    }
}

static void consume(TokenType type, const char *message) {
    if (parser.current.type == type) {
        advance();
        return;
    }

    errorAtCurrent(message);
}

static bool check(TokenType type) {
    return parser.current.type == type;
}

static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

static Stmt *statement();

static Expr *ifStatement(bool canAssign);

static TypeNodeArray genericArgDefinitions();

static Stmt *declaration();

static Expr *expression();

static Value identifierConstant(Token *name);

static ParseRule *getRule(TokenType type);

static Expr *parsePrecedence(Precedence precedence);

static Expr *number(bool canAssign) {
    double value = strtod(parser.previous.start, NULL);
    struct Literal *result = ALLOCATE_NODE(struct Literal, NODE_LITERAL);
    result->value = NUMBER_VAL(value);
    return result;
}

static Expr *unary(bool canAssign) {
    // Parse the operand.
    Token operator = parser.previous;
    Expr *expr = parsePrecedence(PREC_UNARY);

    struct Unary *result = ALLOCATE_NODE(struct Unary, NODE_UNARY);
    result->operator = operator;
    result->right = expr;

    return result;
}

static Expr *list(bool canAssign) {
    ExprArray items;
    initExprArray(&items);
    Token bracket = parser.previous;
    if (!check(TOKEN_RIGHT_BRACKET)) {
        do {
            if (parser.current.type == TOKEN_RIGHT_BRACKET) {
                break;
            }
            Expr *item = expression();
            writeExprArray(&items, item);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_BRACKET, "Expect ']' after list items.");

    struct List *result = ALLOCATE_NODE(struct List, NODE_LIST);
    result->bracket = bracket;
    result->items = items;
    return result;
}

static Expr *map(bool casAssign) {
    ExprArray keys;
    initExprArray(&keys);
    ExprArray values;
    initExprArray(&values);
    Token brace = parser.previous;
    if (!check(TOKEN_RIGHT_BRACE)) {
        do {
            if (parser.current.type == TOKEN_RIGHT_BRACE) {
                break;
            }
            Expr *key = expression();
            writeExprArray(&keys, key);
            consume(TOKEN_COLON, "Expect ':' after map key.");
            Expr *value = expression();
            writeExprArray(&values, value);
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after map items.");

    struct Map *result = ALLOCATE_NODE(struct Map, NODE_MAP);
    result->brace = brace;
    result->keys = keys;
    result->values = values;
    return result;
}

static Expr *binary(Expr *left, bool canAssign) {
    Token operator = parser.previous;
    ParseRule *rule = getRule(operator.type);
    Expr *right = parsePrecedence((Precedence) (rule->precedence + 1));

    struct Binary *result = ALLOCATE_NODE(struct Binary, NODE_BINARY);
    result->operator = operator;
    result->right = right;
    result->left = left;

    return result;
}

static Expr *grouping(bool canAssign) {
    Expr *expr = expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
    return expr;
}

static Expr *string(bool canAssign) {
    Value value = (OBJ_VAL(copyString(parser.previous.start + 1,
                                      parser.previous.length - 2)));

    struct Literal *result = ALLOCATE_NODE(struct Literal, NODE_LITERAL);
    result->value = value;
    return result;
}

static bool identifiersEqual(Token *a, Token *b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static Expr *variable(bool canAssign) {
    Token name = parser.previous;

    if (canAssign && match(TOKEN_EQUAL)) {
        struct Assign *var = ALLOCATE_NODE(struct Assign, NODE_ASSIGN);
        var->name = name;

        var->value = expression();

        return (Expr *) var;
    } else {
        struct Variable *var = ALLOCATE_NODE(struct Variable, NODE_VARIABLE);
        var->name = name;
        return (Expr *) var;
    }
}

static Expr *atom(bool canAssign) {
    ObjAtom *key = copyAtom(parser.previous.start + 1,
                            parser.previous.length - 1);
    struct Literal *result = ALLOCATE_NODE(struct Literal, NODE_LITERAL);
    result->value = OBJ_VAL(key);
    return result;
}

static Expr *literal(bool canAssign) {
    struct Literal *result = ALLOCATE_NODE(struct Literal, NODE_LITERAL);

    switch (parser.previous.type) {
        case TOKEN_FALSE:
            result->value = BOOL_VAL(false);
            break;
        case TOKEN_NIL:
            result->value = NIL_VAL;
            break;
        case TOKEN_TRUE:
            result->value = BOOL_VAL(true);
            break;
    }

    return result;
}

static Expr *and_(Expr *left, bool canAssign) {
    struct Binary *result = ALLOCATE_NODE(struct Binary, NODE_BINARY);
    result->operator = parser.previous;
    result->right = parsePrecedence(PREC_AND);
    result->left = left;// TODO
    return result;
}

static Expr *or_(Expr *left, bool canAssign) {
    struct Binary *result = ALLOCATE_NODE(struct Binary, NODE_BINARY);
    result->operator = parser.previous;
    result->right = parsePrecedence(PREC_OR);
    result->left = left;
    return result;
}

static ExprArray argumentList() {
    ExprArray items;
    initExprArray(&items);

    uint8_t argCount = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            if (parser.current.type == TOKEN_RIGHT_PAREN) {
                break;
            }
            Expr *expr = expression();
            writeExprArray(&items, expr);
            if (argCount == 255) {
                error("Can't have more than 255 arguments.");
            }
            argCount++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after arguments.");
    return items;
}

static Expr *call(Expr *left, bool canAssign) {
    Token paren = parser.previous;
    ExprArray arguments = argumentList();
    struct Call *result = ALLOCATE_NODE(struct Call, NODE_CALL);
    result->paren = paren;
    result->arguments = arguments;
    result->callee = left;
    return (Expr *) result;
}

static Expr *getItem(Expr *left, bool canAssign) {
    Expr *expr = expression();
    struct GetItem *result = ALLOCATE_NODE(struct GetItem, NODE_GETITEM);
    result->object = left;
    result->index = expr;
    consume(TOKEN_RIGHT_BRACKET, "Expect ']' after index.");
    return (Expr *) result;
}

static Expr *pipeCall(Expr *left, bool canAssign) {
    struct Call *result = (struct Call *) parsePrecedence(PREC_CALL);
    if (result->self.self.type != NODE_CALL) {
        errorAtCurrent("Expected functional call after pipe operator!");
        return NULL;
    }

    writeExprArray(&result->arguments, NULL);

    // Move arguments to the right one
    for (int i = 0; i < result->arguments.count - 1; i++) {
        result->arguments.exprs[i + 1] = result->arguments.exprs[i];
    }
    result->arguments.exprs[0] = left;
    return (Expr *) result;
}

static Expr *dot(Expr *left, bool canAssign) {
    consume(TOKEN_IDENTIFIER, "Expect property name after '.'.");
    Token name = parser.previous;

    if (match(TOKEN_EQUAL)) {
        struct Set *result = ALLOCATE_NODE(struct Set, NODE_SET);
        result->object = left;
        result->name = name;
        result->value = expression();
        return result;
    }

    struct Get *result = ALLOCATE_NODE(struct Get, NODE_GET);
    result->object = left;
    result->name = name;
    return result;
}

static Expr *this_(bool canAssign) {
    return variable(false);
}

static Token syntheticToken(const char *text) {
    Token token;
    token.start = text;
    token.length = (int) strlen(text);
    return token;
}

static Expr *super_(bool canAssign) {
    struct Super *result = ALLOCATE_NODE(struct Super, NODE_SUPER);
    result->keyword = parser.previous;

    consume(TOKEN_DOT, "Expect '.' after 'super'.");
    consume(TOKEN_IDENTIFIER, "Expect superclass method name.");

    Token name = parser.previous;
    result->keyword = name;
    result->method = parser.previous;
    return result;
}

static Expr *yield(bool canAssign) {
    struct Yield *result = ALLOCATE_NODE(struct Yield, NODE_YIELD);
    if (!check(TOKEN_SEMICOLON)) {
        result->expression = parsePrecedence(PREC_YIELD);
    }
    return result;
}

static Expr *anonFunction(bool canAssign);

ParseRule parseRules[] = {
        [TOKEN_LEFT_PAREN]    = {grouping, call, PREC_CALL},
        [TOKEN_RIGHT_PAREN]   = {NULL, NULL, PREC_NONE},
        [TOKEN_LEFT_BRACE]    = {map, NULL, PREC_NONE},
        [TOKEN_RIGHT_BRACE]   = {NULL, NULL, PREC_NONE},
        [TOKEN_LEFT_BRACKET]  = {list, getItem, PREC_CALL},
        [TOKEN_RIGHT_BRACKET] = {NULL, NULL, PREC_NONE},
        [TOKEN_PIPE]          = {NULL, pipeCall, PREC_YIELD},
        [TOKEN_COMMA]         = {NULL, NULL, PREC_NONE},
        [TOKEN_DOT]           = {NULL, dot, PREC_CALL},
        [TOKEN_MINUS]         = {unary, binary, PREC_TERM},
        [TOKEN_PLUS]          = {NULL, binary, PREC_TERM},
        [TOKEN_MODULO]        = {NULL, binary, PREC_TERM},
        [TOKEN_SEMICOLON]     = {NULL, NULL, PREC_NONE},
        [TOKEN_SLASH]         = {NULL, binary, PREC_FACTOR},
        [TOKEN_STAR]          = {NULL, binary, PREC_FACTOR},
        [TOKEN_BANG]          = {unary, NULL, PREC_NONE},
        [TOKEN_BANG_EQUAL]    = {NULL, binary, PREC_EQUALITY},
        [TOKEN_EQUAL]         = {NULL, NULL, PREC_NONE},
        [TOKEN_EQUAL_EQUAL]   = {NULL, binary, PREC_EQUALITY},
        [TOKEN_GREATER]       = {NULL, binary, PREC_COMPARISON},
        [TOKEN_GREATER_EQUAL] = {NULL, binary, PREC_COMPARISON},
        [TOKEN_LESS]          = {NULL, binary, PREC_COMPARISON},
        [TOKEN_LESS_EQUAL]    = {NULL, binary, PREC_COMPARISON},
        [TOKEN_IDENTIFIER]    = {variable, NULL, PREC_NONE},
        [TOKEN_ATOM]          = {atom, NULL, PREC_NONE},
        [TOKEN_STRING]        = {string, NULL, PREC_NONE},
        [TOKEN_NUMBER]        = {number, NULL, PREC_NONE},
        [TOKEN_AND]           = {NULL, and_, PREC_AND},
        [TOKEN_CLASS]         = {NULL, NULL, PREC_NONE},
        [TOKEN_ELSE]          = {NULL, NULL, PREC_NONE},
        [TOKEN_FALSE]         = {literal, NULL, PREC_NONE},
        [TOKEN_FOR]           = {NULL, NULL, PREC_NONE},
        [TOKEN_FUN]           = {NULL, NULL, PREC_NONE},
        [TOKEN_IF]            = {ifStatement, NULL, PREC_NONE},
        [TOKEN_NIL]           = {literal, NULL, PREC_NONE},
        [TOKEN_OR]            = {NULL, or_, PREC_OR},
        [TOKEN_RETURN]        = {NULL, NULL, PREC_NONE},
        [TOKEN_SUPER]         = {super_, NULL, PREC_NONE},
        [TOKEN_THIS]          = {this_, NULL, PREC_NONE},
        [TOKEN_TRUE]          = {literal, NULL, PREC_NONE},
        [TOKEN_VAR]           = {NULL, NULL, PREC_NONE},
        [TOKEN_WHILE]         = {NULL, NULL, PREC_NONE},
        [TOKEN_YIELD]         = {yield, NULL, PREC_NONE},
        [TOKEN_AWAIT]         = {NULL, NULL, PREC_NONE},
        [TOKEN_ERROR]         = {NULL, NULL, PREC_NONE},
        [TOKEN_EOF]           = {NULL, NULL, PREC_NONE},
};

static ParseRule *getRule(TokenType type) {
    return &parseRules[type];
}

static Expr *parsePrecedence(Precedence precedence) {
    advance();
    TokenType type = parser.previous.type;
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression.");
        return NULL;
    }

    bool canAssign = precedence <= PREC_ASSIGNMENT;
    Expr *result = prefixRule(canAssign);

    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        InfixParseFn infixRule = getRule(parser.previous.type)->infix;
        result = infixRule(result, canAssign);
    }

    if (canAssign && match(TOKEN_EQUAL)) {
        error("Invalid assignment target.");
    }

    return result;
}


static Token parseVariable(const char *errorMessage) {
    consume(TOKEN_IDENTIFIER, errorMessage);

    return parser.previous;
}

static void beginScope();

static Stmt *block();

static TypeNode *typeAnnotation();

static Expr *anonFunction(bool canAssign) {
    TypeNodeArray generics;
    initTypeNodeArray(&generics);
    if (match(TOKEN_LESS)) {
        generics = genericArgDefinitions();
    }

    consume(TOKEN_LEFT_PAREN, "Expect '(' after fun keyword.");
    ParameterArray params;
    initParameterArray(&params);

    TypeNodeArray types;
    initTypeNodeArray(&types);

    int argCount = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            argCount++;
            if (argCount > 255) {
                errorAtCurrent("Can't have more than 255 parameters.");
            }
            Token name = parseVariable("Expect parameter name.");
            struct Positional *param = ALLOCATE_NODE(struct Positional, NODE_POSITIONAL);

            param->self.name = name;
            writeParameterArray(&params, param);

            if (match(TOKEN_COLON)) {
                TypeNode *typeNode = typeAnnotation();
                writeTypeNodeArray(&types, typeNode);
                param->self.type = typeNode;
            } else {
                writeTypeNodeArray(&types, NULL);
            }
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    TypeNode *returnType = NULL;
    if (match(TOKEN_COLON)) {
        returnType = typeAnnotation();
    }
    consume(TOKEN_ARROW, "Expect '=>' after parameters.");

    struct Block *bl;
    if (match(TOKEN_LEFT_BRACE)) {
        bl = (struct Block *) block();
    } else {
        Expr *expr = expression();

        struct Return *returnNode = ALLOCATE_NODE(struct Return, NODE_RETURN);
        returnNode->value = expr;

        StmtArray stmts;
        initStmtArray(&stmts);
        writeStmtArray(&stmts, (Stmt *) returnNode);

        bl = ALLOCATE_NODE(struct Block, NODE_BLOCK);
        bl->statements = stmts;
    }
    struct Lambda *result = ALLOCATE_NODE(struct Lambda, NODE_LAMBDA);
    result->body = bl->statements;
    result->params = params;
    result->self.type = (TypeNode *) initFunctor(types, returnType, generics);
    return result;
}

static Expr *expression() {
    Expr *result;
    if (match(TOKEN_FUN)) {
        result = anonFunction(false);
    } else {
        result = parsePrecedence(PREC_ASSIGNMENT);
    }
    return result;
}

static Stmt *expressionStatement() {
    struct Expression *result = ALLOCATE_NODE(struct Expression, NODE_EXPRESSION);
    result->self.self.lineno = parser.current.line;
    Expr *expr = expression();
    match(TOKEN_SEMICOLON);
    result->expression = expr;
    return result;
}

static Stmt *block() {
    StmtArray stmts;
    initStmtArray(&stmts);
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        writeStmtArray(&stmts, declaration());
    }

    consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");

    struct Block *result = ALLOCATE_NODE(struct Block, NODE_BLOCK);
    result->statements = stmts;
    return result;
}

static struct Function *function(FunctionType type) {
    TypeNodeArray generics;
    initTypeNodeArray(&generics);
    if (match(TOKEN_LESS)) {
        generics = genericArgDefinitions();
    }

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    ParameterArray params;
    initParameterArray(&params);

    TypeNodeArray types;
    initTypeNodeArray(&types);

    int argCount = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            argCount++;
            if (argCount > 255) {
                errorAtCurrent("Can't have more than 255 parameters.");
            }
            Token name = parseVariable("Expect parameter name.");
            struct Positional *param = ALLOCATE_NODE(struct Positional, NODE_POSITIONAL);

            param->self.name = name;
            writeParameterArray(&params, param);

            if (match(TOKEN_COLON)) {
                TypeNode *typeNode = typeAnnotation();
                writeTypeNodeArray(&types, typeNode);
                param->self.type = typeNode;
            } else {
                writeTypeNodeArray(&types, NULL);
            }
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    TypeNode *returnType = NULL;
    if (match(TOKEN_COLON)) {
        returnType = typeAnnotation();
    }

    consume(TOKEN_LEFT_BRACE, "Expect '{' before function body.");
    struct Block *body = (struct Block *) block();
    struct Function *result = ALLOCATE_NODE(struct Function, NODE_FUNCTION);
    result->body = body->statements;
    result->params = params;
    result->functionType = type;
    result->returnType = returnType;
    result->generics = generics;
    return result;
}

static Expr *ifStatement(bool canAssign) {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
    Expr *condition = expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    Stmt *ifBody = statement();
    Stmt *elseBody = NULL;

    if (match(TOKEN_ELSE)) elseBody = statement();

    struct If *result = ALLOCATE_NODE(struct If, NODE_IF);
    result->thenBranch = ifBody;
    result->elseBranch = elseBody;
    result->condition = condition;
    return result;
}

static Stmt *whileStatement() {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'while'.");
    Expr *condition = expression();
    consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

    Stmt *body = statement();
    struct While *result = ALLOCATE_NODE(struct While, NODE_WHILE);
    result->condition = condition;
    result->body = body;
    return result;
}

static TypeNodeArray genericArgDefinitions() {
    TypeNodeArray generics;
    initTypeNodeArray(&generics);

    if (match(TOKEN_GREATER)) {
        return generics;
    }

    do {
        consume(TOKEN_IDENTIFIER, "Expected identifier in generic argument list.");
        Token name = parser.previous;
        struct TypeDeclaration *result = ALLOCATE_NODE(struct TypeDeclaration, NODE_TYPEDECLARATION);
        result->name = name;
        result->target = NULL;

        if (match(TOKEN_EXTENDS)) {
            TypeNode *argument = typeAnnotation();
            result->target = argument;
        }

        writeTypeNodeArray(&generics, (TypeNode *) result);
    } while (match(TOKEN_COMMA));

    consume(TOKEN_GREATER, "Expected '>' after generic argument list.");

    return generics;
}

static struct Functor *functionTypeAnnotation() {
    struct Functor *result = ALLOCATE_NODE(struct Functor, NODE_FUNCTOR);
    TypeNodeArray arguments;
    initTypeNodeArray(&arguments);
    initTypeNodeArray(&result->generics);

    do {
        TypeNode *type = typeAnnotation();
        writeTypeNodeArray(&arguments, type);
    } while (match(TOKEN_COMMA));

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after functor type arguments.");
    consume(TOKEN_ARROW, "Expect '=>' after functor type arguments.");

    result->returnType = typeAnnotation();
    result->arguments = arguments;

    return (TypeNode *) result;
}

static TypeNode *simpleTypeAnnotation() {
    Token name = parser.previous; // TODO: We don't ever initialize the value arrays...
    // TODO: How is everything still working?

    if (match(TOKEN_LESS)) {
        struct Simple *target = ALLOCATE_NODE(struct Simple, NODE_SIMPLE);
        target->name = name;
        initTypeNodeArray(&target->generics);

        do {
            TypeNode *argument = typeAnnotation();
            writeTypeNodeArray(&target->generics, argument);
        } while (match(TOKEN_COMMA));

        consume(TOKEN_GREATER, "Expect '>' after generic type argument.");
        return (TypeNode *) target;
    } else {
        struct Simple *result = ALLOCATE_NODE(struct Simple, NODE_SIMPLE);
        result->name = name;
        initTypeNodeArray(&result->generics);
        return (TypeNode *) result;
    }
}

static TypeNode *typeAnnotation() {
    TypeNode *leftType;

    if (match(TOKEN_LESS)) {
        TypeNodeArray genericArgs = genericArgDefinitions();
        struct Functor *functor = functionTypeAnnotation();
        functor->generics = genericArgs;
        leftType = (TypeNode *) functor;
    } else if (match(TOKEN_LEFT_PAREN)) {
        leftType = functionTypeAnnotation();
    } else if (match(TOKEN_IDENTIFIER)) {
        leftType = simpleTypeAnnotation();
    } else {
        error("Expect identifier or functor type.");
        return NULL;
    }

    if (!match(TOKEN_BITWISE_OR)) {
        return leftType;
    } else {
        TypeNode *rightType = typeAnnotation();
        struct Union *result = ALLOCATE_NODE(struct Union, NODE_UNION);
        result->left = leftType;
        result->right = rightType;
        return (TypeNode *) result;
    }
}


static Stmt *fieldDeclaration(AssignmentType assignmentType) {
    Token name = parseVariable("Expect variable name.");
    consume(TOKEN_COLON, "Expect type annotation");
    TypeNode *type = typeAnnotation();

    match(TOKEN_SEMICOLON);

    struct Var *var = ALLOCATE_NODE(struct Var, NODE_VAR);
    var->name = name;
    var->initializer = NULL;
    var->type = type;
    var->assignmentType = assignmentType;
    return var;
}

static Stmt *varDeclaration(AssignmentType assignmentType) {
    Token name = parseVariable("Expect variable name.");
    Expr *value = NULL;
    TypeNode *type = NULL;

    if (match(TOKEN_COLON)) {
        type = typeAnnotation();
    }

    if (match(TOKEN_EQUAL)) {
        value = expression();
    }

    if (!type && !value) {
        errorAtCurrent("Var without initializer must provide a type!"); // TODO: Not require this
        return NULL;
    }

    match(TOKEN_SEMICOLON);

    struct Var *var = ALLOCATE_NODE(struct Var, NODE_VAR);
    var->name = name;
    var->initializer = value;
    var->type = type;
    var->assignmentType = assignmentType;
    return var;
}

static Stmt *typeDeclaration() {
    Token name = parseVariable("Expect type name.");
    struct TypeDeclaration *typeDecl = ALLOCATE_NODE(struct TypeDeclaration, NODE_TYPEDECLARATION);
    typeDecl->name = name;

    initTypeNodeArray(&typeDecl->generics);
    if (match(TOKEN_LESS)) {
        typeDecl->generics = genericArgDefinitions();
    }

    consume(TOKEN_EQUAL, "Expect '=' after type name.");

    typeDecl->target = typeAnnotation();
    match(TOKEN_SEMICOLON);

    return typeDecl;
}

static Stmt *forStatement() {
    consume(TOKEN_LEFT_PAREN, "Expect '(' after 'for'.");
    Stmt *initializer = NULL;
    Expr *condition = NULL;
    Expr *increment = NULL;
    if (match(TOKEN_SEMICOLON)) {
        // No initializer.
    } else if (match(TOKEN_VAR)) {
        initializer = varDeclaration(TYPE_VARIABLE);
    } else {
        initializer = expressionStatement();
    }

    if (!match(TOKEN_SEMICOLON)) {
        condition = expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");
    }

    if (!match(TOKEN_RIGHT_PAREN)) {
        increment = expression();
        consume(TOKEN_RIGHT_PAREN, "Expect ')' after for clauses.");
    }

    Stmt *body = statement();
    struct For *result = ALLOCATE_NODE(struct For, NODE_FOR);
    result->initializer = initializer;
    result->condition = condition;
    result->increment = increment;
    result->body = body;
    return result;
}

static Stmt *importStatement() {
    consume(TOKEN_STRING, "Expect '\"' after import.");
    Expr *s = string(false);
    struct Import *result = ALLOCATE_NODE(struct Import, NODE_IMPORT);
    result->expression = s;
    consume(TOKEN_AS, "Expect 'as' after import path.");
    result->name = parseVariable("Expect name after 'as' in import.");
    match(TOKEN_SEMICOLON);
    return result;
}

static Stmt *returnStatement() {
    Token keyword = parser.previous;
    if (match(TOKEN_SEMICOLON)) {
        struct Return *result = ALLOCATE_NODE(struct Return, NODE_RETURN);
        result->value = NULL;
        result->keyword = keyword;
        return (Stmt *) result;
    } else {
        Expr *value = expression();
        match(TOKEN_SEMICOLON);
        struct Return *result = ALLOCATE_NODE(struct Return, NODE_RETURN);
        result->value = value;
        result->keyword = keyword;
        return (Stmt *) result;
    }
}

static Stmt *statement() {
    Stmt *result;
    // if (match(TOKEN_IF)) {
    //        result = ifStatement();
    //    } else
    if (match(TOKEN_RETURN)) {
        result = returnStatement();
    } else if (match(TOKEN_WHILE)) {
        result = whileStatement();
    } else if (match(TOKEN_FOR)) {
        result = forStatement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        result = block();
    } else if (match(TOKEN_IMPORT)) {
        result = importStatement();
    } else {
        result = expressionStatement();
    }

    while (match(TOKEN_SEMICOLON));

    return result;
}

static void synchronize() {
    parser.panicMode = false;

    while (parser.current.type != TOKEN_EOF) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_RETURN:
                return;

            default:; // Do nothing.
        }

        advance();
    }
}

static Stmt *funDeclaration() {
    Token name = parseVariable("Expect function name.");
    struct Function *func = function(TYPE_FUNCTION);
    func->name = name;
    return func;
}

static Stmt *method() {
    consume(TOKEN_FUN, "Expect 'var' or 'fun' keyword.");
    consume(TOKEN_IDENTIFIER, "Expect method name.");
    Token name = parser.previous;
    FunctionType type = TYPE_METHOD;
    if (parser.previous.length == 4 &&
        memcmp(parser.previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }

    struct Function *func = function(type);
    func->name = name;
    return (Stmt *) func;
}

static Stmt *classDeclaration() {
    consume(TOKEN_IDENTIFIER, "Expect class name.");
    Token className = parser.previous;

    TypeNodeArray generics;
    initTypeNodeArray(&generics);
    if (match(TOKEN_LESS)) {
        generics = genericArgDefinitions();
    }

    struct Class *result = ALLOCATE_NODE(struct Class, NODE_CLASS);
    result->name = className;
    result->superclass = NULL;

    if (match(TOKEN_EXTENDS)) {
        consume(TOKEN_IDENTIFIER, "Expect superclass name.");
        struct Variable *var = variable(false);

        if (identifiersEqual(&className, &parser.previous)) {
            error("A class can't inherit from itself.");
        }
        result->superclass = var;
    }

    consume(TOKEN_LEFT_BRACE, "Expect '{' before class body.");

    StmtArray body;
    initStmtArray(&body);
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        if (match(TOKEN_VAR)) {
            writeStmtArray(&body, varDeclaration(TYPE_FIELD));
        } else {
            writeStmtArray(&body, method());
        }
    }

    result->body = body;
    result->generics = generics;
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after class body.");

    return (Stmt *) result;
}

static Stmt *methodSignature() {
    consume(TOKEN_FUN, "Expect 'fun' in interface body.");
    consume(TOKEN_IDENTIFIER, "Expect method name.");
    Token name = parser.previous;

    TypeNodeArray generics;
    initTypeNodeArray(&generics);
    if (match(TOKEN_LESS)) {
        generics = genericArgDefinitions();
    }

    FunctionType type = TYPE_METHOD;
    if (parser.previous.length == 4 &&
        memcmp(parser.previous.start, "init", 4) == 0) {
        type = TYPE_INITIALIZER;
    }

    consume(TOKEN_LEFT_PAREN, "Expect '(' after function name.");

    ParameterArray params;
    initParameterArray(&params);

    TypeNodeArray types;
    initTypeNodeArray(&types);

    int argCount = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            argCount++;
            if (argCount > 255) {
                errorAtCurrent("Can't have more than 255 parameters.");
            }
            Token name = parseVariable("Expect parameter name.");
            struct Positional *param = ALLOCATE_NODE(struct Positional, NODE_POSITIONAL);

            param->self.name = name;
            writeParameterArray(&params, param);

            if (match(TOKEN_COLON)) {
                TypeNode *typeNode = typeAnnotation();
                writeTypeNodeArray(&types, typeNode);
                param->self.type = typeNode;
            } else {
                writeTypeNodeArray(&types, NULL);
            }
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expect ')' after parameters.");

    TypeNode *returnType = NULL;
    if (match(TOKEN_COLON)) {
        returnType = typeAnnotation();
    }

    struct MethodSig *result = ALLOCATE_NODE(struct MethodSig, NODE_METHODSIG);
    result->params = params;
    result->functionType = type;
    result->returnType = returnType;
    result->name = name;
    result->generics = generics;
    return result;
}

static Stmt *interfaceDeclaration() {
    consume(TOKEN_IDENTIFIER, "Expect an interface name.");
    Token interfaceName = parser.previous;

    TypeNodeArray generics;
    initTypeNodeArray(&generics);
    if (match(TOKEN_LESS)) {
        generics = genericArgDefinitions();
    }

    struct Interface *result = ALLOCATE_NODE(struct Interface, NODE_INTERFACE);
    result->name = interfaceName;
    result->superType = NULL;

    if (match(TOKEN_EXTENDS)) {
        consume(TOKEN_IDENTIFIER, "Expect superclass name.");
        struct Variable *var = fieldDeclaration(false);

        if (identifiersEqual(&interfaceName, &parser.previous)) {
            error("An interface can't extend from itself.");
        }
        result->superType = var;
    }

    consume(TOKEN_LEFT_BRACE, "Expect '{' before interface body.");

    StmtArray body;
    initStmtArray(&body);
    while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
        if (match(TOKEN_VAR)) {
            writeStmtArray(&body, varDeclaration(TYPE_FIELD));
        } else {
            writeStmtArray(&body, methodSignature());
        }
    }

    result->body = body;
    result->generics = generics;
    consume(TOKEN_RIGHT_BRACE, "Expect '}' after interface body.");

    return (Stmt *) result;
}

static Stmt *declaration() {
    if (match(TOKEN_CLASS)) {
        return classDeclaration();
    } else if (match(TOKEN_FUN)) {
        return funDeclaration();
    } else if (match(TOKEN_VAR)) {
        return varDeclaration(TYPE_VARIABLE);
    } else if (match(TOKEN_INTERFACE)) {
        return interfaceDeclaration();
    } else if (match(TOKEN_TYPE)) {
        return typeDeclaration();
    } else {
        return statement();
    }

    if (parser.panicMode) synchronize();
}

StmtArray *parseAST(const char *source) {
    initScanner(source);

    parser.hadError = false;
    parser.panicMode = false;

    advance();

    StmtArray *statements = ALLOCATE(StmtArray, 1);
    initStmtArray(statements);

    while (!match(TOKEN_EOF)) {
        writeStmtArray(statements, declaration());
    }

    consume(TOKEN_EOF, "Expect end of expression.");

    return parser.hadError ? NULL : statements;
}
