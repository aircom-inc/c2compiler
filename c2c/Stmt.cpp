/* Copyright 2013 Bas van den Berg
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

#include <stdio.h>
#include <string.h>
#include "Stmt.h"
#include "Expr.h"
#include "StringBuilder.h"
#include "Utils.h"

using namespace C2;
using namespace std;

//#define STMT_DEBUG
#ifdef STMT_DEBUG
static int creationCount;
static int deleteCount;
#endif

Stmt::Stmt() {
#ifdef STMT_DEBUG
    creationCount++;
    fprintf(stderr, "[STMT] create %p  created %d deleted %d\n", this, creationCount, deleteCount);
#endif
}

Stmt::~Stmt() {
#ifdef STMT_DEBUG
    deleteCount++;
    fprintf(stderr, "[STMT] delete %p  created %d deleted %d\n", this, creationCount, deleteCount);
#endif
}

void Stmt::dump() {
    StringBuilder buffer;
    print(0, buffer);
    fprintf(stderr, "%s\n", (const char*)buffer);
}


ReturnStmt::ReturnStmt(Expr* value_)
    : value(value_)
{}

ReturnStmt::~ReturnStmt() {
    delete value;
}

STMT_VISITOR_ACCEPT(ReturnStmt);

void ReturnStmt::print(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "[return]\n";
    if (value) {
        value->print(indent + INDENT, buffer);
    }
}

void ReturnStmt::generateC(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "return";
    if (value) {
        buffer << ' ';
        value->generateC(0, buffer);
    }
    buffer << ";\n";
}


IfStmt::IfStmt(SourceLocation ifLoc,
               Expr* condition, Stmt* thenStmt,
               SourceLocation elseLoc, Stmt* elseStmt)
    : IfLoc(ifLoc)
    , ElseLoc(elseLoc)
{
    SubExprs[VAR] = 0;  // unused?
    SubExprs[COND] = condition;
    SubExprs[THEN] = thenStmt;
    SubExprs[ELSE] = elseStmt;
}

IfStmt::~IfStmt() {
    delete SubExprs[VAR];
    delete SubExprs[COND];
    delete SubExprs[THEN];
    delete SubExprs[ELSE];
}

STMT_VISITOR_ACCEPT(IfStmt);

void IfStmt::print(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "[if]\n";
    SubExprs[COND]->print(indent + INDENT, buffer);
    if (SubExprs[THEN]) SubExprs[THEN]->print(indent + INDENT, buffer);
    if (SubExprs[ELSE]) SubExprs[ELSE]->print(indent + INDENT, buffer);
}

void IfStmt::generateC(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "if (";
    SubExprs[COND]->generateC(0, buffer);
    buffer << ")\n";
    SubExprs[THEN]->generateC(indent, buffer);
    if (SubExprs[ELSE]) {
        buffer.indent(indent);
        buffer << "else\n";
        SubExprs[ELSE]->generateC(indent, buffer);
    }
}


WhileStmt::WhileStmt(SourceLocation Loc_, Expr* Cond_, Stmt* Then_)
    : Loc(Loc_)
    , Cond(Cond_)
    , Then(Then_)
{}

WhileStmt::~WhileStmt() {
    delete Cond;
    delete Then;
}

STMT_VISITOR_ACCEPT(WhileStmt);

void WhileStmt::print(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "[while]\n";
    Cond->print(indent + INDENT, buffer);
    Then->print(indent + INDENT, buffer);
}

void WhileStmt::generateC(int indent, StringBuilder& buffer) {
    printf("%s() TODO\n", __PRETTY_FUNCTION__);
    buffer.indent(indent);
    buffer << "while(";
    Cond->generateC(0, buffer);
    buffer << ")\n";
    Then->generateC(indent, buffer);
}

DoStmt::DoStmt(SourceLocation Loc_, Expr* Cond_, Stmt* Then_)
    : Loc(Loc_)
    , Cond(Cond_)
    , Then(Then_)
{}

DoStmt::~DoStmt() {
    delete Cond;
    delete Then;
}

STMT_VISITOR_ACCEPT(DoStmt);

void DoStmt::print(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "[do]\n";
    Cond->print(indent + INDENT, buffer);
    Then->print(indent + INDENT, buffer);
}

void DoStmt::generateC(int indent, StringBuilder& buffer) {
    printf("%s() TODO\n", __PRETTY_FUNCTION__);
}


BreakStmt::BreakStmt(SourceLocation Loc_) : Loc(Loc_) {}

BreakStmt::~BreakStmt() {}

STMT_VISITOR_ACCEPT(BreakStmt);

void BreakStmt::print(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "[break]\n";
}

void BreakStmt::generateC(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "break;\n";
}


ContinueStmt::ContinueStmt(SourceLocation Loc_) : Loc(Loc_) {}

ContinueStmt::~ContinueStmt() {}

STMT_VISITOR_ACCEPT(ContinueStmt);

void ContinueStmt::print(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "[continue]\n";
}

void ContinueStmt::generateC(int indent, StringBuilder& buffer) {
    fprintf(stderr, "HEREHER\n");
    buffer.indent(indent);
    buffer << "continue;\n";
}


CompoundStmt::CompoundStmt(SourceLocation l, SourceLocation r, StmtList& stmts)
    : NumStmts(stmts.size())
    , Body(0)
    , Left(l)
    , Right(r)
{
    if (NumStmts != 0) {
        Body = new Stmt*[NumStmts];
        // TODO improve
        for (int i=0; i<NumStmts; i++) Body[i] = stmts[i];
    }
}

CompoundStmt::~CompoundStmt() {
    for (int i=0; i<NumStmts; i++) delete Body[i];
    delete[] Body;
}

STMT_VISITOR_ACCEPT(CompoundStmt);

void CompoundStmt::print(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "[compound]\n";
    for (int i=0; i<NumStmts; i++) {
#ifdef STMT_DEBUG
        fprintf(stderr, "[STMT] CompoundStmt::print() child=%p\n", Body[i]);
#endif
        Body[i]->print(indent + INDENT, buffer);
    }
}

void CompoundStmt::generateC(int indent, StringBuilder& buffer) {
    buffer.indent(indent);
    buffer << "{\n";
    for (int i=0; i<NumStmts; i++) {
        Body[i]->generateC(indent + INDENT, buffer);
    }
    buffer.indent(indent);
    buffer << "}\n";
}
