from contextlib import redirect_stdout

exprs = [
    "Expr     : Node self, TypeNode *type",
    "Binary   : Expr *left, Token operator, Expr* right",
    "Grouping : Expr* expression",
    "Literal  : Value value",
    "Unary    : Token operator, Expr* right",
    "Variable : Token name",
    "AltAssign: Token name, Token operator, Expr* value",
    "Assign   : Token name, Expr* value",
    "Logical  : Expr* left, Token operator, Expr* right",
    "Call     : Expr* callee, Token paren, ExprArray arguments",
    "GetItem  : Expr* object, Token bracket, Expr* index",
    "Get      : Expr* object, Token name",
    "Set      : Expr* object, Token name, Expr* value",
    "Super    : Token keyword, Token method",
    "This     : Token keyword",
    "Yield    : Expr* expression",
    "Lambda   : ParameterArray params, StmtArray body, TypeNodeArray generics",
    "List     : ExprArray items, Token bracket",
    "Map      : ExprArray keys, ExprArray values, Token brace",
    # "Arguments: Expr* items",
    # "Parameters: "
]

stmts = [
    "Stmt       : Node self",
    "Expression : Expr* expression, TypeNode* type",
    "Var        : Token name, Expr* initializer, TypeNode *type, AssignmentType assignmentType",
    "Block      : StmtArray statements",
    "Function   : Token name, ParameterArray params, TypeNodeArray generics," +
    " StmtArray body, FunctionType functionType, TypeNode *returnType",
    "Class      : Token name, struct Variable* superclass," +
    " StmtArray body, TypeNodeArray generics",
    "If         : Expr* condition, Stmt* thenBranch," +
    " Stmt* elseBranch",
    "While      : Expr* condition, Stmt* body",
    "For        : Stmt* initializer, Expr* condition, Expr* increment, Stmt* body",
    "Break      : Token keyword",
    "Return     : Token keyword, Expr* value",
    "Import     : Expr* expression, Token name",
    "Enum       : Token name, StmtArray body",
    "EnumItem   : Token name, ParameterArray params",
    "MethodSig  : Token name, ParameterArray params, TypeNode *returnType,"
    " FunctionType functionType, TypeNodeArray generics",
]

type_items = [
    "TypeNode         : Node self",
    "Simple           : Token name, TypeNodeArray generics",
    "Functor          : TypeNodeArray arguments, TypeNode *returnType, TypeNodeArray generics",
    "Union            : TypeNode* left, TypeNode* right",
    "Interface        : Token name, struct Variable* superType, StmtArray body, TypeNodeArray generics",
    "TypeDeclaration  : Token name, TypeNode* target, TypeNodeArray generics",
]

param_types = [
    "Parameter  : Node self, Token name, TypeNode* type",
    "Positional : ",
    "Keyword    : Expr* default_",
    "Variadic   : ",
]

# TODO: Argument list, etc

capfirst = lambda x: x[0].upper() + x[1:]

types = dict(typeNode=type_items, expr=exprs, stmt=stmts, parameter=param_types)

file = open("../ast/ast.c", "w")
with redirect_stdout(file):
    print('#include "ast.h"')
    print()

    for group, items in types.items():
        titleGroup = capfirst(group)
        print(f"""
void init{titleGroup}Array({titleGroup}Array* {group}Array) {{
    {group}Array->count = 0;
    {group}Array->capacity = 0;
    {group}Array->{group}s = NULL;
}}      
        
void write{titleGroup}Array({titleGroup}Array * {group}Array, {titleGroup}* {group}) {{
    if ({group}Array->capacity < {group}Array->count + 1) {{
        int oldCapacity = {group}Array->capacity;
        {group}Array->capacity = GROW_CAPACITY(oldCapacity);
        {group}Array->{group}s = GROW_ARRAY({titleGroup}*, {group}Array->{group}s,
                                       oldCapacity, {group}Array->capacity);
    }}

    {group}Array->{group}s[{group}Array->count] = {group};
    {group}Array->count++;
}}

void free{titleGroup}Array({titleGroup}Array * {group}Array) {{
    FREE_ARRAY({titleGroup}*, {group}Array->{group}s, {group}Array->capacity);
    init{titleGroup}Array({group}Array);
}}
""")
file = open("../ast/ast.h", "w")
with redirect_stdout(file):
    all_types = []

    print("""#ifndef saffron_AST_H
#define saffron_AST_H""")
    print('#include "../scanner.h"')
    print('#include "../value.h"')
    print('#include "../memory.h"')
    print()

    print("""typedef enum {
    TYPE_FUNCTION,
    TYPE_SCRIPT,
    TYPE_METHOD,
    TYPE_INITIALIZER,
} FunctionType;
""")

    print("""typedef enum {
    TYPE_FIELD,
    TYPE_VARIABLE,
} AssignmentType;
""")

    print('#define ALLOCATE_NODE(type, nodeType) (type*) allocateNode(sizeof(type), nodeType)')
    print()

    for group, items in types.items():
        for item in items[1:]:
            name, args = item.split(":")
            name = name.strip()
            all_types.append(name)

    print("typedef enum {")
    for node_type in all_types:
        print(f"    NODE_{node_type.upper()},")
    print("} NodeType;")
    print()

    print("typedef struct {")
    print(f"    NodeType type;")
    print(f"    int lineno;")
    print(f"    bool isMarked;")
    print(f"    struct Node *next;")
    print("} Node;")
    print()
    print("Node *allocateNode(size_t size, NodeType type);")
    print()

    for group, items in types.items():
        titleGroup = capfirst(group)

        item = items[0]
        name, args = item.split(":")
        name = name.strip()
        args = args.split(",")

        print("typedef struct {")
        for arg in args:
            print(f"    {arg.strip()};")

        print("}", f"{capfirst(name)};")
        print()

        print("typedef struct {")
        print(f"    int count;")
        print(f"    int capacity;")
        print(f"    {capfirst(group)}** {group}s;")
        print("}", f"{capfirst(group)}Array;")
        print()

        print(f'void init{titleGroup}Array({titleGroup}Array* {group}Array);')
        print(f'void write{titleGroup}Array({titleGroup}Array * {group}Array, {titleGroup}* {group});')
        print(f'void free{titleGroup}Array({titleGroup}Array * {group}Array);')
        print()

    for group, items in types.items():
        for item in items[1:]:
            name, args = item.split(":")
            name = name.strip()
            args = args.split(",")

            print(f"struct {capfirst(name)}", "{")
            print(f"    {capfirst(group)} self;")
            for arg in args:
                print(f"    {arg.strip()};")

            print("};")
            print()

    print("#endif //saffron_AST_H")

# TODO: Write free function to recursively free tree
