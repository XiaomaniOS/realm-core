%skeleton "lalr1.cc" /* -*- C++ -*- */
%require "3.4"
%defines
// %no-lines

%define api.token.constructor
%define api.value.type variant
%define parse.assert

%code requires {
  # include <string>
  # include "realm/query_expression.hpp"
  namespace realm {
  class ParserDriver;
  }
  using namespace realm;
}

// The parsing context.
%param { realm::ParserDriver& drv }

%locations

%define parse.trace
%define parse.error verbose

%code {
#include "realm/query/driver.hpp"
}

%define api.token.prefix {TOK_}
%token
  END  0  "end of file"
  TRUEPREDICATE "true"
  FALSEPREDICATE "false"
  ASSIGN  ":="
  EQUALS  "=="
  LESS    "<"
  GREATER ">"
  GREATER_EQUAL ">="
  LESS_EQUAL    "<="
  AND     "&&"
  OR      "||"
  MINUS   "-"
  PLUS    "+"
  STAR    "*"
  SLASH   "/"
  LPAREN  "("
  RPAREN  ")"
  NOT     "!"
  DOT     "."
;

%token <std::string> IDENTIFIER "identifier"
%token <std::string> STRING "string"
%token <int64_t> NUMBER "number"
%token <double> FLOAT "float"
%type  <std::unique_ptr<realm::Subexpr>> exp
%type  <realm::Query> pred
%type  <realm::LinkChain> path
%type  <std::string> path_elem

%printer { util::serializer::SerialisationState state; yyo << $$->description(state); } <std::unique_ptr<realm::Subexpr>>;
%printer { yyo << $$.get_description(); } <realm::Query>;
%printer { yyo << "LinkChain"; } <realm::LinkChain>;
%printer { yyo << $$; } <*>;
%printer { yyo << "<>"; } <>;

%%
%start unit;
unit: pred  {
  drv.result = $1;
};

%left "||";
%left "&&";
%left "+" "-";
%left "*" "/";
%right "!";

exp:
  NUMBER            { $$.reset(new realm::Value<int64_t>($1)); }
| STRING            { $$.reset(new ConstantStringValue($1)); }
| FLOAT             { $$.reset(new realm::Value<double>($1)); }
| path IDENTIFIER   { $$.reset($1.column($2)); }
| "(" exp ")"       { $$ = std::move($2); }

pred:
  exp "==" exp      {
                        std::unique_ptr<realm::Subexpr> l;
                        std::unique_ptr<realm::Subexpr> r;
                        if ($1->has_constant_evaluation()) {
                            l = std::move($1);
                            r = std::move($3);
                        }
                        else {
                            l = std::move($3);
                            r = std::move($1);
                        }
                        $$ = Query(std::unique_ptr<Expression>(new Compare<Equal>(std::move(l), std::move(r))));
                    }
| exp "<" exp       { 
                        $$ = Query(std::unique_ptr<Expression>(new Compare<Less>(std::move($1), std::move($3))));
                    }
| exp ">" exp       { 
                        $$ = Query(std::unique_ptr<Expression>(new Compare<Greater>(std::move($1), std::move($3))));
                    }
| exp "<=" exp      { 
                        $$ = Query(std::unique_ptr<Expression>(new Compare<LessEqual>(std::move($1), std::move($3))));
                    }
| exp ">=" exp      { 
                        $$ = Query(std::unique_ptr<Expression>(new Compare<GreaterEqual>(std::move($1), std::move($3))));
                    }
| pred "&&" pred    {
                        $$ = $1 && $3;
                    }
| pred "||" pred    {
                        $$ = $1 || $3;
                    }
| "(" pred ")"      { $$ = std::move($2); }
| "true"            {
                        Query q = drv.base_table->where();
                        q.and_query(std::unique_ptr<realm::Expression>(new TrueExpression));
                        $$ = std::move(q);
                    }
| "false"           {
                        Query q = drv.base_table->where();
                        q.and_query(std::unique_ptr<realm::Expression>(new FalseExpression));
                        $$ = std::move(q);
                    }
| "!" pred          {
                        Query q = drv.base_table->where();
                        q.Not();
                        q.and_query($2);
                        $$ = std::move(q);
                    }

path:
  %empty            { $$ = LinkChain(drv.base_table); }
| path path_elem    { $1.link($2); $$ = $1; }  

path_elem:
  IDENTIFIER "."    { $$ = $1; }
%%

void
yy::parser::error (const location_type& l, const std::string& m)
{
    std::ostringstream ostr;
    ostr << l << ": " << m;
    drv.error(ostr.str());
}
