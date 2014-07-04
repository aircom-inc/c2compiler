/* Copyright 2013,2014 Bas van den Berg
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>
#include <set>
// TODO REMOVE
#include <stdio.h>

// for SmallString
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
// for tool_output_file
//#include <llvm/Support/ToolOutputFile.h>

#include "CGenerator/CCodeGenerator.h"
#include "AST/Module.h"
#include "AST/AST.h"
#include "AST/Decl.h"
#include "AST/Expr.h"
#include "FileUtils/FileUtils.h"
#include "Utils/StringBuilder.h"
#include "Utils/color.h"
#include "Utils/constants.h"
#include "Utils/GenUtils.h"

//#define CCODE_DEBUG
#ifdef CCODE_DEBUG
#include <iostream>
#define LOG_FUNC std::cerr << ANSI_BLUE << __func__ << "()" << ANSI_NORMAL << "\n";
#define LOG_DECL(_d) std::cerr << ANSI_BLUE << __func__ << "() " << ANSI_YELLOW  << _d->getName()<< ANSI_NORMAL << "\n";
#else
#define LOG_FUNC
#define LOG_DECL(_d)
#endif


using namespace C2;
using namespace llvm;
using namespace clang;

CCodeGenerator::CCodeGenerator(const std::string& filename_, Mode mode_, const Modules& modules_, bool prefix)
    : filename(filename_)
    , curmod(0)
    , mode(mode_)
    , no_local_prefix(prefix)
    , modules(modules_)
{
    hfilename = filename + ".h";
    cfilename = filename + ".c";
}

CCodeGenerator::~CCodeGenerator() {
}

void CCodeGenerator::generate() {
    // emit include guard
    hbuf << "#ifndef ";
    GenUtils::toCapital(filename, hbuf);
    hbuf << "_H\n";
    hbuf << "#define ";
    GenUtils::toCapital(filename, hbuf);
    hbuf << "_H\n";
    hbuf << '\n';

    // generate all includes
    EmitIncludes();

    // generate types
    for (EntriesIter iter = entries.begin(); iter != entries.end(); ++iter) {
        AST* ast = *iter;
        curmod = &ast->getModuleName();
        for (unsigned i=0; i<ast->numTypes(); i++) {
            EmitTypeDecl(ast->getType(i));
        }
        curmod = 0;
    }

    // generate variables
    for (EntriesIter iter = entries.begin(); iter != entries.end(); ++iter) {
        AST* ast = *iter;
        curmod = &ast->getModuleName();
        for (unsigned i=0; i<ast->numVars(); i++) {
            EmitVariable(ast->getVar(i));
        }
        curmod = 0;
    }
    // TODO Arrayvalues

    // generate functions
    for (EntriesIter iter = entries.begin(); iter != entries.end(); ++iter) {
        AST* ast = *iter;
        curmod = &ast->getModuleName();
        for (unsigned i=0; i<ast->numFunctions(); i++) {
            EmitFunction(ast->getFunction(i));
        }
        curmod = 0;
    }

    // emit end of include guard
    hbuf << "#endif\n";
}

void CCodeGenerator::EmitExpr(Expr* E, StringBuilder& output) {
    LOG_FUNC
    switch (E->getKind()) {
    case EXPR_INTEGER_LITERAL:
        {
            IntegerLiteral* N = cast<IntegerLiteral>(E);
            output << (int) N->Value.getSExtValue();
            return;
        }
    case EXPR_FLOAT_LITERAL:
        {
            FloatingLiteral* F = cast<FloatingLiteral>(E);
            char temp[20];
            sprintf(temp, "%f", F->Value.convertToFloat());
            output << temp;
            return;
        }
    case EXPR_BOOL_LITERAL:
        {
            BooleanLiteral* B = cast<BooleanLiteral>(E);
            cbuf << (int)B->getValue();
            return;
        }
    case EXPR_CHAR_LITERAL:
        {
            CharacterLiteral* C = cast<CharacterLiteral>(E);
            output << '\'' << (char)C->getValue() << '\'';
            return;
        }
    case EXPR_STRING_LITERAL:
        {
            StringLiteral* S = cast<StringLiteral>(E);
            EmitStringLiteral(S->value, output);
            return;
        }
    case EXPR_NIL:
        output << "NULL";
        return;
    case EXPR_CALL:
        EmitCallExpr(E, output);
        return;
    case EXPR_IDENTIFIER:
        EmitIdentifierExpr(E, output);
        return;
    case EXPR_INITLIST:
        {
            InitListExpr* I = cast<InitListExpr>(E);
            output << "{ ";
            const ExprList& values = I->getValues();
            for (unsigned i=0; i<values.size(); i++) {
                if (i == 0 && values[0]->getKind() == EXPR_INITLIST) output << '\n';
                EmitExpr(values[i], output);
                if (i != values.size() -1) output << ", ";
                if (values[i]->getKind() == EXPR_INITLIST) output << '\n';
            }
            output << " }";
            return;
        }
    case EXPR_TYPE:
        {
            TypeExpr* T = cast<TypeExpr>(E);
            EmitTypePreName(T->getType(), output);
            EmitTypePostName(T->getType(), output);
            return;
        }
    case EXPR_DECL:
        EmitDeclExpr(cast<DeclExpr>(E), output, 0);
        return;
    case EXPR_BINOP:
        EmitBinaryOperator(E, output);
        return;
    case EXPR_CONDOP:
        EmitConditionalOperator(E, output);
        return;
    case EXPR_UNARYOP:
        EmitUnaryOperator(E, output);
        return;
    case EXPR_BUILTIN:
        {
            BuiltinExpr* S = cast<BuiltinExpr>(E);
            if (S->isSizeof()) {
                output << "sizeof(";
                EmitExpr(S->getExpr(), output);
                output << ')';
            } else {
                IdentifierExpr* I = cast<IdentifierExpr>(S->getExpr());
                Decl* D = I->getDecl();
                // should be VarDecl(for array/enum) or TypeDecl(array/enum)
                switch (D->getKind()) {
                case DECL_FUNC:
                    assert(0);
                    break;
                case DECL_VAR:
                    {
                        VarDecl* VD = cast<VarDecl>(D);
                        QualType Q = VD->getType();
                        if (Q.isArrayType()) {
                            // generate: sizeof(array) / sizeof(array[0]);
                            output << "sizeof(" << I->getName() << ")/sizeof(" << I->getName() << "[0])";
                            return;
                        }
                        // TODO also allow elemsof for EnumType
                        // NOTE cannot be converted to C if used with enums
                        assert(0 && "TODO");
                        return;
                    }
                case DECL_ENUMVALUE:
                    break;
                case DECL_ALIASTYPE:
                case DECL_STRUCTTYPE:
                case DECL_ENUMTYPE:
                case DECL_FUNCTIONTYPE:
                case DECL_ARRAYVALUE:
                case DECL_IMPORT:
                    assert(0);
                    break;
                }
            }
            return;
        }
    case EXPR_ARRAYSUBSCRIPT:
        {
            ArraySubscriptExpr* A = cast<ArraySubscriptExpr>(E);
            EmitExpr(A->getBase(), output);
            output << '[';
            EmitExpr(A->getIndex(), output);
            output << ']';
            return;
        }
    case EXPR_MEMBER:
        EmitMemberExpr(E, output);
        return;
    case EXPR_PAREN:
        {
            ParenExpr* P = cast<ParenExpr>(E);
            cbuf << '(';
            EmitExpr(P->getExpr(), cbuf);
            cbuf << ')';
            return;
        }
    }
}

void CCodeGenerator::EmitBinaryOperator(Expr* E, StringBuilder& output) {
    LOG_FUNC
    BinaryOperator* B = cast<BinaryOperator>(E);
    EmitExpr(B->getLHS(), output);
    output << ' ' << BinaryOperator::OpCode2str(B->getOpcode()) << ' ';
    EmitExpr(B->getRHS(), output);
}

void CCodeGenerator::EmitConditionalOperator(Expr* E, StringBuilder& output) {
    LOG_FUNC
    ConditionalOperator* C = cast<ConditionalOperator>(E);
    EmitExpr(C->getCond(), output);
    output << " ? ";
    EmitExpr(C->getLHS(), output);
    output << " : ";
    EmitExpr(C->getRHS(), output);

}

void CCodeGenerator::EmitUnaryOperator(Expr* E, StringBuilder& output) {
    LOG_FUNC
    UnaryOperator* U = cast<UnaryOperator>(E);

    switch (U->getOpcode()) {
    case UO_PostInc:
    case UO_PostDec:
        EmitExpr(U->getExpr(), output);
        output << UnaryOperator::OpCode2str(U->getOpcode());
        break;
    case UO_PreInc:
    case UO_PreDec:
    case UO_AddrOf:
    case UO_Deref:
    case UO_Plus:
    case UO_Minus:
    case UO_Not:
    case UO_LNot:
        //output.indent(indent);
        output << UnaryOperator::OpCode2str(U->getOpcode());
        EmitExpr(U->getExpr(), output);
        break;
    default:
        assert(0);
    }
}

void CCodeGenerator::EmitMemberExpr(Expr* E, StringBuilder& output) {
    LOG_FUNC
    MemberExpr* M = cast<MemberExpr>(E);
    if (M->isModulePrefix()) {
        // A.B where A is a module
        EmitDecl(M->getDecl(), output);
    } else {
        // A.B where A is decl of struct/union type
        EmitExpr(M->getBase(), cbuf);
        if (M->isArrow()) cbuf << "->";
        else cbuf << '.';
        cbuf << M->getMemberName();
    }
}

void CCodeGenerator::EmitDeclExpr(DeclExpr* E, StringBuilder& output, unsigned indent) {
    LOG_FUNC
    output.indent(indent);
    if (E->hasLocalQualifier()) output << "static ";
    EmitTypePreName(E->getDeclType(), output);
    output << ' ';
    output << E->getName();
    EmitTypePostName(E->getDeclType(), output);
    if (E->getInitValue()) {
        output << " = ";
        EmitExpr(E->getInitValue(), output);
    }
}

void CCodeGenerator::EmitCallExpr(Expr* E, StringBuilder& output) {
    LOG_FUNC
    CallExpr* C = cast<CallExpr>(E);
    EmitExpr(C->getFn(), output);
    output << '(';
    for (unsigned i=0; i<C->numArgs(); i++) {
        if (i != 0) output << ", ";
        EmitExpr(C->getArg(i), output);
    }
    output << ')';
}

void CCodeGenerator::EmitIdentifierExpr(Expr* E, StringBuilder& output) {
    LOG_FUNC
    IdentifierExpr* I = cast<IdentifierExpr>(E);
    EmitDecl(I->getDecl(), output);
}

void CCodeGenerator::EmitDecl(const Decl* D, StringBuilder& output) {
    assert(D);

    if (D->getModule()) {
        addPrefix(D->getModule()->getCName(), D->getName(), output);
    } else {
        output << D->getName();
    }
}

void CCodeGenerator::dump() {
    printf("---- code for %s ----\n%s\n", hfilename.c_str(), (const char*)hbuf);
    printf("---- code for %s ----\n%s\n", cfilename.c_str(), (const char*)cbuf);
}

void CCodeGenerator::write(const std::string& target, const std::string& name) {
    // write C files to output/<target>/<module>.{c,h}
    StringBuilder outname(128);
    outname << "output/" << target << '/';

    FileUtils::writeFile(outname, std::string(outname) + cfilename, cbuf);
    FileUtils::writeFile(outname, std::string(outname) + hfilename, hbuf);
}

void CCodeGenerator::EmitIncludes() {
    typedef std::set<std::string> StringList;
    typedef StringList::const_iterator StringListConstIter;
    StringList systemIncludes;
    StringList localIncludes;

    // filter out unique entries, split into system and local includes
    for (EntriesIter iter = entries.begin(); iter != entries.end(); ++iter) {
        AST* ast = *iter;
        for (unsigned i=0; i<ast->numImports(); i++) {
            const ImportDecl* D = ast->getImport(i);
            ModulesConstIter iter = modules.find(D->getModuleName());
            assert(iter != modules.end());
            const Module* P = iter->second;

            if (P->isPlainC()) {
                systemIncludes.insert(P->getName());
                continue;
            }
            if (mode == MULTI_FILE) {
                localIncludes.insert(P->getName());
            }
        }
    }

    for (StringListConstIter iter = systemIncludes.begin(); iter != systemIncludes.end(); ++iter) {
        cbuf << "#include <" << *iter << ".h>\n";
    }
    for (StringListConstIter iter = localIncludes.begin(); iter != localIncludes.end(); ++iter) {
        cbuf << "#include \"" << *iter << ".h\"\n";
    }
    cbuf << '\n';
}


void CCodeGenerator::EmitFunction(FunctionDecl* F) {
    LOG_DECL(F)
    if (mode == SINGLE_FILE) {
        // emit all function protos as forward declarations in header
        EmitFunctionProto(F, hbuf);
        hbuf << ";\n\n";
    } else {
        if (F->isPublic()) {
            EmitFunctionProto(F, hbuf);
            hbuf << ";\n\n";
        } else {
            cbuf << "static ";
        }
    }

    EmitFunctionProto(F, cbuf);
    cbuf << ' ';
    EmitCompoundStmt(F->getBody(), 0, false);
    cbuf << '\n';
}

void CCodeGenerator::EmitFunctionArgs(FunctionDecl* F, StringBuilder& output) {
    LOG_DECL(F)
    output << '(';
    int count = F->numArgs();
    if (F->isVariadic()) count++;
    for (unsigned i=0; i<F->numArgs(); i++) {
        VarDecl* A = F->getArg(i);
        EmitVarDecl(A, output, 0);
        if (count != 1) output << ", ";
        count--;
    }
    if (F->isVariadic()) output << "...";
    output << ')';
}

void CCodeGenerator::EmitVariable(VarDecl* V) {
    LOG_DECL(V)
    if (V->isPublic() && mode != SINGLE_FILE) {
        // TODO type
        hbuf << "extern ";
        EmitTypePreName(V->getType(), hbuf);
        hbuf << ' ';
        addPrefix(*curmod, V->getName(), hbuf);
        EmitTypePostName(V->getType(), hbuf);
        // TODO add space if needed (on StringBuilder)
        hbuf << ";\n";
        hbuf << '\n';
    } else {
        cbuf << "static ";
    }
    EmitTypePreName(V->getType(), cbuf);
    cbuf << ' ';
    addPrefix(*curmod, V->getName(), cbuf);
    EmitTypePostName(V->getType(), cbuf);
    if (V->getInitValue()) {
        cbuf << " = ";
        EmitExpr(V->getInitValue(), cbuf);
    }
#if 0
    const VarDecl::InitValues& inits = V->getIncrValues();
    if (inits.size()) {
        cbuf << " = {\n";
        VarDecl::InitValuesConstIter iter=inits.begin();
        while (iter != inits.end()) {
            const ArrayValueDecl* E = (*iter);
            cbuf.indent(INDENT);
            EmitExpr(E->getExpr(), cbuf);
            cbuf << ",\n";
            ++iter;
        }
        cbuf << '}';
    }
#endif
    cbuf << ";\n";
    cbuf << '\n';
}

void CCodeGenerator::EmitTypeDecl(TypeDecl* T) {
    LOG_DECL(T)

    StringBuilder* out = &cbuf;
    if (T->isPublic()) out = &hbuf;
    switch (T->getKind()) {
    case DECL_FUNC:
    case DECL_VAR:
    case DECL_ENUMVALUE:
        assert(0);
        break;
    case DECL_ALIASTYPE:
        *out << "typedef ";
        EmitTypePreName(T->getType(), *out);
        *out << ' ';
        addPrefix(*curmod, T->getName(), *out);
        EmitTypePostName(T->getType(), *out);
        *out << ";\n\n";
        break;
    case DECL_STRUCTTYPE:
        EmitStructType(cast<StructTypeDecl>(T), T->isPublic() ? hbuf : cbuf, 0);
        return;
    case DECL_ENUMTYPE:
        EmitEnumType(cast<EnumTypeDecl>(T), T->isPublic() ? hbuf : cbuf);
        return;
    case DECL_FUNCTIONTYPE:
        EmitFunctionType(cast<FunctionTypeDecl>(T), T->isPublic() ? hbuf : cbuf);
        return;
    case DECL_ARRAYVALUE:
    case DECL_IMPORT:
        assert(0);
        break;
    }
}

void CCodeGenerator::EmitStructType(StructTypeDecl* S, StringBuilder& out, unsigned indent) {
    LOG_DECL(S)
    out.indent(indent);
    if (S->isGlobal()) out << "typedef ";
    out << (S->isStruct() ? "struct " : "union ");
    out << "{\n";
    for (unsigned i=0;i<S->numMembers(); i++) {
        Decl* member = S->getMember(i);
        if (isa<VarDecl>(member)) {
            EmitVarDecl(cast<VarDecl>(member), out, indent + INDENT);
            out << ";\n";
        } else if (isa<StructTypeDecl>(member)) {
            EmitStructType(cast<StructTypeDecl>(member), out, indent+INDENT);
        } else {
            assert(0);
        }
    }
    out.indent(indent);
    out << '}';
    if (S->getName() != "") out << ' ' << S->getName();
    out << ";\n";
    if (S->isGlobal()) out << '\n';
}

void CCodeGenerator::EmitEnumType(EnumTypeDecl* E, StringBuilder& output) {
    LOG_DECL(E)
    output << "typedef enum {\n";
    for (unsigned i=0; i<E->numConstants(); i++) {
        EnumConstantDecl* C = E->getConstant(i);
        output.indent(INDENT);
        addPrefix(*curmod, C->getName(), output);
        if (C->getInitValue()) {
            output << " = ";
            EmitExpr(C->getInitValue(), output);
        }
        output << ",\n";
    }
    output << "} " << E->getName() << ";\n\n";
}

// output: typedef void (*name)(args);
void CCodeGenerator::EmitFunctionType(FunctionTypeDecl* FTD, StringBuilder& output) {
    LOG_DECL(FTD)
    FunctionDecl* F = FTD->getDecl();
    output << "typedef ";
    EmitTypePreName(F->getReturnType(), output);
    EmitTypePostName(F->getReturnType(), output);
    output << " (*" << F->getName() << ')';
    EmitFunctionArgs(F, output);
    output << ";\n\n";
}

void CCodeGenerator::EmitVarDecl(VarDecl* D, StringBuilder& output, unsigned indent) {
    LOG_DECL(D)
    output.indent(indent);
    //if (D->hasLocalQualifier()) output << "static ";
    EmitTypePreName(D->getType(), output);
    output << ' ';
    output << D->getName();
    EmitTypePostName(D->getType(), output);
    if (D->getInitValue()) {
        output << " = ";
        EmitExpr(D->getInitValue(), output);
    }
    //output << ";\n";
}

void CCodeGenerator::EmitStmt(Stmt* S, unsigned indent) {
    LOG_FUNC
    switch (S->getKind()) {
    case STMT_RETURN:
        {
            ReturnStmt* R = cast<ReturnStmt>(S);
            cbuf.indent(indent);
            cbuf << "return";
            if (R->getExpr()) {
                cbuf << ' ';
                EmitExpr(R->getExpr(), cbuf);
            }
            cbuf << ";\n";
            return;
        }
    case STMT_EXPR:
        {
            Expr* E = cast<Expr>(S);
            cbuf.indent(indent);
            EmitExpr(E, cbuf);
            cbuf << ";\n";
            return;
        }
    case STMT_IF:
        EmitIfStmt(S, indent);
        return;
    case STMT_WHILE:
        EmitWhileStmt(S, indent);
        return;
    case STMT_DO:
        EmitDoStmt(S, indent);
        return;
    case STMT_FOR:
        EmitForStmt(S, indent);
        return;
    case STMT_SWITCH:
        EmitSwitchStmt(S, indent);
        return;
    case STMT_CASE:
    case STMT_DEFAULT:
        assert(0 && "Should already be generated");
        break;
    case STMT_BREAK:
        cbuf.indent(indent);
        cbuf << "break;\n";
        return;
    case STMT_CONTINUE:
        cbuf.indent(indent);
        cbuf << "continue;\n";
        return;
    case STMT_LABEL:
        {
            LabelStmt* L = cast<LabelStmt>(S);
            cbuf << L->getName();
            cbuf << ":\n";
            EmitStmt(L->getSubStmt(), indent);
            return;
        }
    case STMT_GOTO:
        {
            GotoStmt* G = cast<GotoStmt>(S);
            cbuf.indent(indent);
            cbuf << "goto " << G->getName() << ";\n";
            return;
        }
    case STMT_COMPOUND:
        {
            CompoundStmt* C = cast<CompoundStmt>(S);
            EmitCompoundStmt(C, indent, true);
            return;
        }
    }
}

void CCodeGenerator::EmitCompoundStmt(CompoundStmt* C, unsigned indent, bool startOnNewLine) {
    LOG_FUNC
    if (startOnNewLine) cbuf.indent(indent);
    cbuf << "{\n";
    const StmtList& Stmts = C->getStmts();
    for (unsigned i=0; i<Stmts.size(); i++) {
        EmitStmt(Stmts[i], indent + INDENT);
    }
    cbuf.indent(indent);
    cbuf << "}\n";
}

void CCodeGenerator::EmitIfStmt(Stmt* S, unsigned indent) {
    LOG_FUNC
    IfStmt* I = cast<IfStmt>(S);
    cbuf.indent(indent);
    cbuf << "if (";
    EmitExpr(I->getCond(), cbuf);
    cbuf << ")\n";
    EmitStmt(I->getThen(), indent);
    if (I->getElse()) {
        cbuf.indent(indent);
        cbuf << "else\n";
        EmitStmt(I->getElse(), indent);
    }
}

void CCodeGenerator::EmitWhileStmt(Stmt* S, unsigned indent) {
    LOG_FUNC
    WhileStmt* W = cast<WhileStmt>(S);
    cbuf.indent(indent);
    cbuf << "while (";
    // TEMP, assume Expr
    Expr* E = cast<Expr>(W->getCond());
    EmitExpr(E, cbuf);
    cbuf << ") ";
    Stmt* Body = W->getBody();
    if (Body->getKind() == STMT_COMPOUND) {
        CompoundStmt* C = cast<CompoundStmt>(Body);
        EmitCompoundStmt(C, indent, false);
    } else {
        EmitStmt(Body, 0);
    }
}

void CCodeGenerator::EmitDoStmt(Stmt* S, unsigned indent) {
    LOG_FUNC
    DoStmt* D = cast<DoStmt>(S);
    cbuf.indent(indent);
    cbuf << "do ";
    Stmt* Body = D->getBody();
    if (Body->getKind() == STMT_COMPOUND) {
        CompoundStmt* C = cast<CompoundStmt>(Body);
        EmitCompoundStmt(C, indent, false);
    } else {
        EmitStmt(Body, 0);
    }
    cbuf.indent(indent);
    // TODO add after '}'
    cbuf << "while (";
    // TEMP, assume Expr
    Expr* E = cast<Expr>(D->getCond());
    EmitExpr(E, cbuf);
    cbuf << ");\n";
}

void CCodeGenerator::EmitForStmt(Stmt* S, unsigned indent) {
    LOG_FUNC
    ForStmt* F = cast<ForStmt>(S);
    cbuf.indent(indent);
    cbuf << "for (";
    Stmt* Init = F->getInit();
    if (Init) {
        // assume Expr
        EmitExpr(cast<Expr>(Init), cbuf);
    }
    cbuf << ';';

    Expr* Cond = F->getCond();
    if (Cond) {
        cbuf << ' ';
        EmitExpr(Cond, cbuf);
    }
    cbuf << ';';

    Expr* Incr = F->getIncr();
    if (Incr) {
        cbuf << ' ';
        EmitExpr(Incr, cbuf);
    }

    cbuf << ") ";
    Stmt* Body = F->getBody();
    if (Body->getKind() == STMT_COMPOUND) {
        EmitCompoundStmt(cast<CompoundStmt>(Body), indent, false);
    } else {
        EmitStmt(Body, 0);
    }
}

void CCodeGenerator::EmitSwitchStmt(Stmt* S, unsigned indent) {
    LOG_FUNC
    SwitchStmt* SW = cast<SwitchStmt>(S);
    cbuf.indent(indent);
    cbuf << "switch (";
    EmitExpr(SW->getCond(), cbuf);
    cbuf << ") {\n";

    const StmtList& Cases = SW->getCases();
    for (unsigned i=0; i<Cases.size(); i++) {
        Stmt* Case = Cases[i];
        switch (Case->getKind()) {
        case STMT_CASE:
            {
                CaseStmt* C = cast<CaseStmt>(Case);
                cbuf.indent(indent + INDENT);
                cbuf << "case ";
                EmitExpr(C->getCond(), cbuf);
                cbuf << ":\n";
                const StmtList& Stmts = C->getStmts();
                for (unsigned s=0; s<Stmts.size(); s++) {
                    EmitStmt(Stmts[s], indent + INDENT + INDENT);
                }
                break;
            }
        case STMT_DEFAULT:
            {
                DefaultStmt* D = cast<DefaultStmt>(Case);
                cbuf.indent(indent + INDENT);
                cbuf << "default:\n";
                const StmtList& Stmts = D->getStmts();
                for (unsigned s=0; s<Stmts.size(); s++) {
                    EmitStmt(Stmts[s], indent + INDENT + INDENT);
                }
                break;
            }
        default:
            assert(0);
        }
    }

    cbuf.indent(indent);
    cbuf << "}\n";
}

void CCodeGenerator::EmitFunctionProto(FunctionDecl* F, StringBuilder& output) {
    LOG_FUNC
    if (mode == SINGLE_FILE && F->getName() != "main") output << "static ";
    EmitTypePreName(F->getReturnType(), output);
    EmitTypePostName(F->getReturnType(), output);
    output << ' ';
    addPrefix(*curmod, F->getName(), output);
    EmitFunctionArgs(F, output);
}

static const char* builtin2cname[] = {
    "char",             // Int8
    "short",            // Int16
    "int",              // Int32
    "long long",        // Int64
    "unsigned char",    // UInt8
    "unsigned short",   // UInt16
    "unsigned int",     // UInt32
    "unsigned long long",// UInt64
    "float",            // Float32
    "double",           // Float64
    "int",              // Bool
    "void",             // Void
};

void CCodeGenerator::EmitTypePreName(QualType type, StringBuilder& output) {
    LOG_FUNC
    if (type.isConstQualified()) output << "const ";
    const Type* T = type.getTypePtr();
    switch (T->getTypeClass()) {
    case TC_BUILTIN:
        {
            // TODO handle Qualifiers
            const BuiltinType* BI = cast<BuiltinType>(T);
            output << builtin2cname[BI->getKind()];
            break;
        }
    case TC_POINTER:
        // TODO handle Qualifiers
        EmitTypePreName(cast<PointerType>(T)->getPointeeType(), output);
        output << '*';
        break;
    case TC_ARRAY:
        // TODO handle Qualifiers
        EmitTypePreName(cast<ArrayType>(T)->getElementType(), output);
        break;
    case TC_UNRESOLVED:
        // TODO handle Qualifiers?
        {
            const UnresolvedType* U = cast<UnresolvedType>(T);
            U->printLiteral(output);
        }
        break;
    case TC_ALIAS:
        EmitTypePreName(cast<AliasType>(T)->getRefType(), output);
        break;
    case TC_STRUCT:
        {
            const StructType* ST = cast<StructType>(T);
            output << ST->getDecl()->getName();
            break;
        }
    case TC_ENUM:
        {
            const EnumType* ET = cast<EnumType>(T);
            output << ET->getDecl()->getName();
            break;
        }
    case TC_FUNCTION:
        output << cast<FunctionType>(T)->getDecl()->getName();
        break;
    case TC_PACKAGE:
        assert(0 && "TODO");
        break;
    }
}

void CCodeGenerator::EmitTypePostName(QualType type, StringBuilder& output) {
    LOG_FUNC
    if (type.isArrayType()) {
        // TEMP, use canonical type, since type can be AliasType
        type = type.getCanonicalType();
        const ArrayType* A = cast<ArrayType>(type);
        EmitTypePostName(A->getElementType(), output);
        output << '[';
        if (A->getSizeExpr()) {
            EmitExpr(A->getSizeExpr(), output);
        }
        output << ']';
    }
}

void CCodeGenerator::EmitStringLiteral(const std::string& input, StringBuilder& output) {
    LOG_FUNC
    output << '"';
    const char* cp = input.c_str();
    for (unsigned i=0; i<input.size(); i++) {
        switch (*cp) {
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        case '\033':
            output << "\\033";
            break;
        // TODO other escaped chars
        default:
            output << *cp;
            break;
        }
        cp++;
    }
    output << '"';
}

void CCodeGenerator::addPrefix(const std::string& modName, const std::string& name, StringBuilder& buffer) const {
    if (modName.empty()) {
        buffer << name;
        return;
    }
    if (no_local_prefix && modName == *curmod) {
        buffer << name;
        return;
    }
    GenUtils::addName(modName, name, buffer);
}

