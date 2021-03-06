#include "parser.h"
#include "compiler.h"
#include "target.h"
#include "yyparser.h"
#include <llvm/IR/Verifier.h>          //for verifying basic structure of functions
#include <llvm/Bitcode/ReaderWriter.h> //for r/w when outputting bitcode
#include <llvm/Support/FileSystem.h>   //for r/w when outputting bitcode
#include <llvm/Support/raw_ostream.h>  //for ostream when outputting bitcode
#include "llvm/Transforms/Scalar.h"    //for most passes
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Linker/Linker.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/GenericValue.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace llvm;


/* 
 * Skips input in a given istream until it encounters the given coordinates,
 * with each newline signalling the end of a row.
 *
 * precondition: coordinates must be valid
 */
void skipToCoords(istream& ifs, unsigned int row, unsigned int col){
    unsigned int line = 1;
    if(line != row){
        while(true){
            char c = ifs.get();
            if(c == '\n'){
                line++;
                if(line >= row){
                    c = 0;
                    break;
                }
            }else if(c == EOF){
                break;
            }
        }
    }
}

/*
 *  Prints a given line (row) of a file, along with an arrow pointing to
 *  the specified column.
 */
void printErrLine(yy::location& loc){
    if(!loc.begin.filename) return;
    ifstream f{*loc.begin.filename};

    //Premature newline error, show previous line as error instead
    if(loc.begin.column == 0) loc.begin.line--;

    //skip to line in question
    skipToCoords(f, loc.begin.line, loc.begin.column);

    //print line
    string s;
    getline(f, s);
    if(loc.begin.column == 0) loc.begin.column = s.length() + 1;
    cout << s;

    //draw arrow
    putchar('\n');
    cout << "\033[;31m"; //red
    unsigned int i = 1;

    //skip to begin pos
    for(; i < loc.begin.column; i++) putchar(' ');

    //draw arrow until end pos
    for(; i <= loc.end.column; i++) putchar('^');

    cout << "\033[;m"; //reset color
}


void ante::error(const char* msg, yy::location& loc){
    if(loc.begin.filename)
        cout << "\033[;3m" << *loc.begin.filename << "\033[;m: ";
    else
        cout << "\033[;3m(unknown file)\033[;m: ";

    cout << "\033[;1m" << loc.begin.line << ",";
    if(loc.begin.column == loc.end.column)
        cout << loc.begin.column << "\033[;0m";
    else
        cout << loc.begin.column << '-' << loc.end.column << "\033[;0m";

    cout << "\t\033[;31merror: \033[;m" <<  msg << endl;
    printErrLine(loc);
    cout << endl << endl;
}


/*
 *  Inform the user of an error and return nullptr.
 *  (perhaps this should throw an exception?)
 */
TypedValue* Compiler::compErr(string msg, yy::location& loc){
    error(msg.c_str(), loc);
    errFlag = true;
    return nullptr;
}


/*
 *  Returns amount of values in a tuple, from 0 to max uint.
 *  Assumes argument is a tuple.
 *  
 *  Doubles as a getNodesInBlock function, but does not
 *  count child nodes.
 */
size_t Compiler::getTupleSize(Node *tup){
    size_t size = 0;
    while(tup){
        tup = tup->next.get();
        size++;
    }
    return size;
}



/*
 *  Compiles a statement list and returns its last statement.
 */
TypedValue* compileStmtList(Node *nList, Compiler *c){
    TypedValue *ret = nullptr;
    while(nList){
        ret = nList->compile(c);
        nList = nList->next.get();
    }
    return ret;
}


bool isUnsignedTypeTag(const TypeTag tt){
    return tt==TT_U8||tt==TT_U16||tt==TT_U32||tt==TT_U64||tt==TT_Usz;
}


TypedValue* IntLitNode::compile(Compiler *c){
    return new TypedValue(ConstantInt::get(getGlobalContext(),
                            APInt(getBitWidthOfTypeTag(type), 
                            atol(val.c_str()), isUnsignedTypeTag(type))), mkAnonTypeNode(type));
}


const fltSemantics& typeTagToFltSemantics(TypeTag tokTy){
    switch(tokTy){
        case TT_F16: return APFloat::IEEEhalf;
        case TT_F32: return APFloat::IEEEsingle;
        case TT_F64: return APFloat::IEEEdouble;
        default:     return APFloat::IEEEdouble;
    }
}

TypedValue* FltLitNode::compile(Compiler *c){
    return new TypedValue(ConstantFP::get(getGlobalContext(), APFloat(typeTagToFltSemantics(type), val.c_str())), mkAnonTypeNode(type));
}


TypedValue* BoolLitNode::compile(Compiler *c){
    return new TypedValue(ConstantInt::get(getGlobalContext(), APInt(1, (bool)val, true)), mkAnonTypeNode(TT_Bool));
}


TypedValue* ModNode::compile(Compiler *c){
    return nullptr;
}


TypedValue* TypeNode::compile(Compiler *c){
    //check for enum value
    if(type == TT_Data || type == TT_TaggedUnion){
        auto *dataTy = c->lookupType(typeName);
        if(!dataTy) return 0;

        auto *unionDataTy = c->lookupType(dataTy->getParentUnionName());
        if(!unionDataTy) return 0;

        Value *tag = ConstantInt::get(getGlobalContext(), APInt(8, unionDataTy->getTagVal(typeName), true));
        auto *ty = deepCopyTypeNode(unionDataTy->tyn.get());

        Type *unionTy = c->typeNodeToLlvmType(ty);
        Type *curTy = tag->getType();

        //allocate for the largest possible union member
        auto *alloca = c->builder.CreateAlloca(unionTy);

        //but make sure to bitcast it to the current member before storing an incorrect type
        Value *castTo = c->builder.CreateBitCast(alloca, curTy->getPointerTo());
        c->builder.CreateStore(tag, castTo);

        //load the initial alloca, not the bitcasted one
        Value *unionVal = c->builder.CreateLoad(alloca);
        ty->type = TT_TaggedUnion;
        return new TypedValue(unionVal, ty);
    }
    return nullptr;
}


TypedValue* StrLitNode::compile(Compiler *c){
    TypeNode *strty = mkAnonTypeNode(TT_Data);
    strty->typeName = "Str";
    auto *ptr = c->builder.CreateGlobalStringPtr(val);

    auto* tupleTy = StructType::get(getGlobalContext(), {Type::getInt8PtrTy(getGlobalContext()), Type::getInt32Ty(getGlobalContext())});
    Constant* strarr[] = {UndefValue::get(Type::getInt8PtrTy(getGlobalContext())), ConstantInt::get(getGlobalContext(), APInt(8, val.length(), true))};

    auto *uninitStr = ConstantStruct::get(tupleTy, strarr);
    auto *str = c->builder.CreateInsertValue(uninitStr, ptr, 0);

    return new TypedValue(str, strty);
}

TypedValue* CharLitNode::compile(Compiler *c){
    return new TypedValue(ConstantInt::get(getGlobalContext(), APInt(8, val, true)), mkAnonTypeNode(TT_C8));
}


TypedValue* ArrayNode::compile(Compiler *c){
    vector<Constant*> arr;
    TypeNode *tyn = mkAnonTypeNode(TT_Array);

    for(Node *n : exprs){
        auto *tval = n->compile(c);
        arr.push_back((Constant*)tval->val);

        if(!tyn->extTy.get())
            tyn->extTy.reset(tval->type.get());
    }

    auto *ty = ArrayType::get(arr[0]->getType(), exprs.size());
    auto *val = ConstantArray::get(ty, arr);
    return new TypedValue(c->builder.CreateConstGEP1_32(val, 0), tyn);
}

TypedValue* Compiler::getVoidLiteral(){
    vector<Constant*> elems;
    vector<Type*> elemTys;
    Value* tuple = ConstantStruct::get(StructType::get(getGlobalContext(), elemTys), elems);
    return new TypedValue(tuple, mkAnonTypeNode(TT_Void));
}

TypedValue* TupleNode::compile(Compiler *c){
    vector<Constant*> elems;
    elems.reserve(exprs.size());

    vector<Type*> elemTys;
    elemTys.reserve(exprs.size());

    map<unsigned, Value*> pathogenVals;
    TypeNode *tyn = mkAnonTypeNode(TT_Tuple);

    TypeNode *cur = 0;

    //Compile every value in the tuple, and if it is not constant,
    //add it to pathogenVals
    for(unsigned i = 0; i < exprs.size(); i++){
        auto *tval = exprs[i]->compile(c);
        if(dynamic_cast<Constant*>(tval->val)){
            elems.push_back((Constant*)tval->val);
        }else{
            pathogenVals[i] = tval->val;
            elems.push_back(UndefValue::get(tval->getType()));
        }
        elemTys.push_back(tval->getType());

        if(cur){
            //cannot just do a swap here because unique_ptr<TypeNode> 
            //cannot swap with a unique_ptr<Node>
            cur->next.release();
            cur->next.reset(tval->type.get());
            tval->type.release();
            cur = (TypeNode*)cur->next.get();
        }else{
            tyn->extTy.reset(tval->type.get());
            cur = tyn->extTy.get();
        }
    }

    //Create the constant tuple with undef values in place for the non-constant values
    Value* tuple = ConstantStruct::get(StructType::get(getGlobalContext(), elemTys), elems);

    //Insert each pathogen value into the tuple individually
    for(auto it = pathogenVals.cbegin(); it != pathogenVals.cend(); it++){
        tuple = c->builder.CreateInsertValue(tuple, it->second, it->first);
    }

    //A void value is represented by the empty tuple, ()
    if(exprs.size() == 0){
        tyn->type = TT_Void;
    }
   
    return new TypedValue(tuple, tyn);
}


vector<TypedValue*> TupleNode::unpack(Compiler *c){
    vector<TypedValue*> ret;
    for(Node *n : exprs){
        auto *tv = n->compile(c);
        if(tv && tv->type->type != TT_Void)
            ret.push_back(tv);
    }
    return ret;
}


/*
 *  When a retnode is compiled within a block, care must be taken to not
 *  forcibly insert the branch instruction afterwards as it leads to dead code.
 */
TypedValue* RetNode::compile(Compiler *c){
    TypedValue *ret = expr->compile(c);
    if(!ret) return 0;
    
    /*Function *f =*/ c->builder.GetInsertBlock()->getParent();

    /*
    if(!llvmTypeEq(ret->getType(), f->getReturnType())){
        return c->compErr("return expression of type " + llvmTypeToStr(ret->getType()) +
               " does not match function return type " + llvmTypeToStr(f->getReturnType()), this->loc);
    }*/

    return new TypedValue(c->builder.CreateRet(ret->val), ret->type);
}


TypedValue* ImportNode::compile(Compiler *c){
    if(!dynamic_cast<StrLitNode*>(expr.get())) return 0;

    c->importFile(((StrLitNode*)expr.get())->val.c_str());
    return c->getVoidLiteral();
}


TypedValue* WhileNode::compile(Compiler *c){
    Function *f = c->builder.GetInsertBlock()->getParent();
    BasicBlock *cond  = BasicBlock::Create(getGlobalContext(), "while_cond", f);
    BasicBlock *begin = BasicBlock::Create(getGlobalContext(), "while", f);
    BasicBlock *end   = BasicBlock::Create(getGlobalContext(), "end_while", f);

    c->builder.CreateBr(cond);
    c->builder.SetInsertPoint(cond);
    auto *condval = condition->compile(c);
    c->builder.CreateCondBr(condval->val, begin, end);

    c->builder.SetInsertPoint(begin);
    auto *val = child->compile(c); //compile the while loop's body

    if(!val) return 0;
    if(!dynamic_cast<ReturnInst*>(val->val))
        c->builder.CreateBr(cond);
    
    c->builder.SetInsertPoint(end);
    return c->getVoidLiteral();
}


TypedValue* ForNode::compile(Compiler *c){
    assert(false && "For loops are still unimplemented.");

    Function *f = c->builder.GetInsertBlock()->getParent();
    BasicBlock *cond  = BasicBlock::Create(getGlobalContext(), "for_cond", f);
    BasicBlock *begin = BasicBlock::Create(getGlobalContext(), "for", f);
    BasicBlock *end   = BasicBlock::Create(getGlobalContext(), "end_for", f);

    c->builder.CreateBr(cond);
    c->builder.SetInsertPoint(cond);
    auto *condval = range->compile(c);
    c->builder.CreateCondBr(condval->val, begin, end);

    c->builder.SetInsertPoint(begin);
    auto *val = child->compile(c); //compile the while loop's body

    if(!val) return 0;
    if(!dynamic_cast<ReturnInst*>(val->val))
        c->builder.CreateBr(cond);
    
    c->builder.SetInsertPoint(end);
    return c->getVoidLiteral();
}

//create a new scope if the user indents
TypedValue* BlockNode::compile(Compiler *c){
    c->enterNewScope();
    TypedValue *ret = block->compile(c);
    c->exitScope();
    return ret;
}


//Since parameters are managed in Compiler::compfn, this need not do anything
TypedValue* NamedValNode::compile(Compiler *c)
{ return nullptr; }


/*
 *  Loads a variable from the stack
 */
TypedValue* VarNode::compile(Compiler *c){
    auto *var = c->lookup(name);

    if(var){
        return dynamic_cast<AllocaInst*>(var->getVal()) ?
            new TypedValue(c->builder.CreateLoad(var->getVal(), name), var->tval->type)
            : var->tval;
    }else{
        auto *fn = c->getFunction(name);

        return fn? fn : c->compErr("Variable or function '" + name + "' has not been declared.", this->loc);
    }
}


TypedValue* LetBindingNode::compile(Compiler *c){
    TypedValue *val = expr->compile(c);
    if(!val) return nullptr;

    TypeNode *tyNode;
    if((tyNode = (TypeNode*)typeExpr.get())){
        if(!llvmTypeEq(val->val->getType(), c->typeNodeToLlvmType(tyNode))){
            return c->compErr("Incompatible types in explicit binding.", expr->loc);
        }
    }

    bool nofree = true;//val->type->type != TT_Ptr || dynamic_cast<Constant*>(val->val);
    c->stoVar(name, new Variable(name, val, c->scope, nofree));
    
    return val;
}

TypedValue* compVarDeclWithInferredType(VarDeclNode *node, Compiler *c){
    TypedValue *val = node->expr->compile(c);
    if(!val) return nullptr;
        
    TypedValue *alloca = new TypedValue(c->builder.CreateAlloca(val->getType(), 0, node->name.c_str()), val->type);
    val = new TypedValue(c->builder.CreateStore(val->val, alloca->val), val->type);

    bool nofree = true;//val->type->type != TT_Ptr || dynamic_cast<Constant*>(val->val);
    c->stoVar(node->name, new Variable(node->name, alloca, c->scope, nofree));
    return val;
}

TypedValue* VarDeclNode::compile(Compiler *c){
    //check for redeclaration, but only on topmost scope
    Variable *redeclare;
    try{
        redeclare = c->varTable.back()->at(this->name);
    }catch(out_of_range r){
        redeclare = 0;
    }

    if(redeclare)
        return c->compErr("Variable " + name + " was redeclared.", this->loc);

    //check for an inferred type
    TypeNode *tyNode = (TypeNode*)typeExpr.get();
    if(!tyNode) return compVarDeclWithInferredType(this, c);

    Type *ty = c->typeNodeToLlvmType(tyNode);
    TypedValue *alloca = new TypedValue(c->builder.CreateAlloca(ty, 0, name.c_str()), tyNode);

    Variable *var = new Variable(name, alloca, c->scope);
    c->stoVar(name, var);
    if(expr.get()){
        TypedValue *val = expr->compile(c);
        if(!val) return 0;

        var->noFree = true;//var->getType() != TT_Ptr || dynamic_cast<Constant*>(val->val);
        
        //Make sure the assigned value matches the variable's type
        if(!llvmTypeEq(alloca->getType()->getPointerElementType(), val->getType())){
            return c->compErr("Cannot assign expression of type " + llvmTypeToStr(val->getType())
                        + " to a variable of type " + llvmTypeToStr(alloca->getType()->getPointerElementType()),
                        expr->loc);
        }

        return new TypedValue(c->builder.CreateStore(val->val, alloca->val), tyNode);
    }else{
        return alloca;
    }
}

/*
 *  Simple wrapper function for compInsert to insert into a named field
 *  instead of an index
 */
TypedValue* compFieldInsert(Compiler *c, BinOpNode *bop, Node *expr){
    VarNode *field = static_cast<VarNode*>(bop->rval.get());

    //A . operator can also have a type/module as its lval, but its
    //impossible to insert into a non-value so fail if the lvalue is one
    if(auto *tn = dynamic_cast<TypeNode*>(bop->lval.get()))
        return c->compErr("Cannot insert value into static module '" + typeNodeToStr(tn), tn->loc);

   
    Value *val;
    TypeNode *tyn;

    //prevent l from being used after this scope; only val and tyn should be used as only they
    //are updated with the automatic pointer dereferences.
    { 
        auto *l = bop->lval->compile(c);
        if(!l) return 0;
    
        val = l->val;
        tyn = l->type.get();
        
        if(!dynamic_cast<LoadInst*>(l->val))
            return c->compErr("Variable must be mutable to be assigned to, but instead is an immutable " +
                    typeNodeToStr(tyn), bop->loc);
    }

    //the . operator automatically dereferences pointers, so update val and tyn accordingly.
    while(tyn->type == TT_Ptr){
        val = c->builder.CreateLoad(val);
        tyn = tyn->extTy.get();
    }

    //this is the variable that will store the changes after the later insertion
    Value *var = static_cast<LoadInst*>(val)->getPointerOperand();

    //check to see if this is a field index
    if(tyn->type == TT_Data || tyn->type == TT_Tuple){
        auto dataTy = c->lookupType(typeNodeToStr(tyn));

        if(dataTy){
            auto index = dataTy->getFieldIndex(field->name);

            if(index != -1){
                TypeNode *indexTy = dataTy->tyn->extTy.get();

                auto *newval = expr->compile(c);
                if(!newval) return 0;

                if(*indexTy != *newval->type)
                    return c->compErr("Cannot assign expression of type " + typeNodeToStr(newval->type.get()) +
                           " to a variable of type " + typeNodeToStr(indexTy), expr->loc);


                auto *ins = c->builder.CreateInsertValue(val, newval->val, index);
                
                c->builder.CreateStore(ins, var);
                return c->getVoidLiteral();
            }
        }
    }

    return c->compErr("Method/Field " + field->name + " not found in type " + typeNodeToStr(tyn), bop->loc);
}

TypedValue* VarAssignNode::compile(Compiler *c){
    //If this is an insert value (where the lval resembles var[index] = ...)
    //then this must be instead compiled with compInsert, otherwise the [ operator
    //would retrieve the value at the index instead of the reference for storage.
    if(BinOpNode *bop = dynamic_cast<BinOpNode*>(ref_expr)){
        if(bop->op == '#')
            return c->compInsert(bop, expr.get());
        else if(bop->op == '.')
            return compFieldInsert(c, bop, expr.get());
    }

    //otherwise, this is just a normal assign to a variable
    TypedValue *tmp = ref_expr->compile(c);
    if(!tmp) return 0;

    if(!dynamic_cast<LoadInst*>(tmp->val))
        return c->compErr("Variable must be mutable to be assigned to, but instead is an immutable " +
                llvmTypeToStr(tmp->getType()), ref_expr->loc);
    
    Value *dest = ((LoadInst*)tmp->val)->getPointerOperand();
    
    //compile the expression to store
    TypedValue *assignExpr = expr->compile(c);
    
    //Check for errors before continuing
    if(!assignExpr) return 0;

    //lvalue must compile to a pointer for storage, usually an alloca value
    if(!PointerType::isLoadableOrStorableType(tmp->getType())){
        return c->compErr("Attempted assign without a memory address, with type "
                + llvmTypeToStr(tmp->getType()), ref_expr->loc);
    }

    //and finally, make sure the assigned value matches the variable's type
    if(!llvmTypeEq(tmp->getType(), assignExpr->getType())){
        return c->compErr("Cannot assign expression of type " + llvmTypeToStr(assignExpr->getType())
                    + " to a variable of type " + llvmTypeToStr(tmp->getType()),
                    expr->loc);
    }

    //now actually create the store
    c->builder.CreateStore(assignExpr->val, dest);

    //all assignments return a void value
    return c->getVoidLiteral();
}


/*
 * Translates a NamedValNode list to a vector
 * of the types it contains.  If the list contains
 * a varargs type (represented by the absence of a type)
 * then a nullptr is inserted for that parameter.
 */
vector<Type*> getParamTypes(Compiler *c, NamedValNode *nvn, size_t paramCount){
    vector<Type*> paramTys;
    paramTys.reserve(paramCount);

    for(size_t i = 0; i < paramCount && nvn; i++){

        TypeNode *paramTyNode = (TypeNode*)nvn->typeExpr.get();
        if(paramTyNode)
            paramTys.push_back(c->typeNodeToLlvmType(paramTyNode));
        else
            paramTys.push_back(nullptr); //terminating null = varargs function
        nvn = (NamedValNode*)nvn->next.get();
    }
    return paramTys;
}


TypedValue* Compiler::compLetBindingFn(FuncDeclNode *fdn, size_t nParams, vector<Type*> &paramTys, unsigned int scope){
    FunctionType *preFnTy = FunctionType::get(Type::getVoidTy(getGlobalContext()), paramTys, fdn->varargs);

    //preFn is the predecessor to fn because we do not yet know its return type, so its body must be compiled,
    //then the type must be checked and the new function with correct return type created, and their bodies swapped.
    Function *preFn = Function::Create(preFnTy, Function::ExternalLinkage, "__lambda_pre__", module.get());
    
    //Create the entry point for the function
    BasicBlock *caller = builder.GetInsertBlock();
    BasicBlock *bb = BasicBlock::Create(getGlobalContext(), "entry", preFn);
    builder.SetInsertPoint(bb);
 
    TypeNode *fnTyn = mkAnonTypeNode(TT_Function);
    TypeNode *curTyn = 0;

    //tell the compiler to create a new scope on the stack.
    enterNewScope();

    //iterate through each parameter and add its value to the new scope.
    NamedValNode *cParam = fdn->params.get();
    vector<Value*> preArgs;
    for(auto &arg : preFn->args()){
        TypeNode *paramTyNode = (TypeNode*)cParam->typeExpr.get();

        stoVar(cParam->name, new Variable(cParam->name, new TypedValue(&arg, paramTyNode), scope));
        preArgs.push_back(&arg);

        if(curTyn){
            curTyn->next.reset(paramTyNode);
            curTyn = (TypeNode*)curTyn->next.get();
        }else{
            fnTyn->extTy.reset(paramTyNode);
            curTyn = fnTyn->extTy.get();
        }
        if(!(cParam = (NamedValNode*)cParam->next.get())) break;
    }

    //actually compile the function, and hold onto the last value
    TypedValue *v = fdn->child->compile(this);
    //End of the function, discard the function's scope.
    exitScope();

    //llvm requires explicit returns, so generate a return even if
    //the user did not in their function.
    if(!dynamic_cast<ReturnInst*>(v->val)){
        if(v->type->type == TT_Void)
            builder.CreateRetVoid();
        else
            builder.CreateRet(v->val);
    }

    //create the actual function's type, along with the function itself.
    FunctionType *ft = FunctionType::get(v->getType(), paramTys, fdn->varargs);
    Function *f = Function::Create(ft, Function::ExternalLinkage, fdn->name.length() > 0 ? fdn->name : "__lambda__", module.get());
   
    //prepend the ret type to the function's type node node extension list.
    //(A typenode represents functions by having the first extTy as the ret type,
    //and the (optional) next types in the list as the parameter types)
    curTyn = fnTyn->extTy.get();
    fnTyn->extTy.release();
    TypeNode *retTy = deepCopyTypeNode(v->type.get());
    retTy->next.reset(curTyn);
    fnTyn->extTy.reset(retTy);

    //finally, swap the bodies of the two functions and delete the former.
    f->getBasicBlockList().push_back(&preFn->front());
    preFn->removeFromParent();
    
    //swap all instances of preFn's parameters with f's parameters
    int i = 0;
    for(auto &arg : f->args()){
        preArgs[i++]->replaceAllUsesWith(&arg);
    }

    auto *ret = new TypedValue(f, fnTyn);

    //only store the function if it has a name (and thus is not a lambda function)
    if(fdn->name.length() > 0)
        stoVar(fdn->name, new Variable(fdn->name, ret, scope));

    builder.SetInsertPoint(caller);
    return ret;
}


vector<Argument*> buildArguments(FunctionType *ft){
    vector<Argument*> args;
    for(unsigned i = 0, e = ft->getNumParams(); i != e; i++){
        assert(!ft->getParamType(i)->isVoidTy() && "Cannot have void typed arguments!");
        args.push_back(new Argument(ft->getParamType(i)));
    }
    return args;
}


/*
 *  Translates a list of NamedValNodes to a list of TypeNodes
 *  that are deep copies of each named val node's type
 */
TypeNode* createFnTyNode(NamedValNode *params, TypeNode *retTy){
    TypeNode *fnTy = mkAnonTypeNode(TT_Function);
    fnTy->extTy.reset(retTy ? deepCopyTypeNode(retTy) : mkAnonTypeNode(TT_Void));

    TypeNode *curTyn = fnTy->extTy.get();
    while(params && params->typeExpr.get()){
        curTyn->next.reset(deepCopyTypeNode((TypeNode*)params->typeExpr.get()));
        curTyn = (TypeNode*)curTyn->next.get();
        params = (NamedValNode*)params->next.get();
    }
    return fnTy;
}

/*
 *  Handles a compiler directive (eg. ![inline]) then compiles the function fdn
 *  with either compFn or compLetBindingFn.
 */
TypedValue* compPreProcFn(Compiler *c, FuncDeclNode *fdn, unsigned int scope, PreProcNode *ppn){
    fdn->modifiers.release();
    fdn->modifiers.reset(ppn->next.get());
    auto *fn = c->compFn(fdn, scope);
    if(!fn) return 0;

    if(VarNode *vn = dynamic_cast<VarNode*>(ppn->expr.get())){
        if(vn->name == "inline"){
            ((Function*)fn->val)->addFnAttr("always_inline");
        }else if(vn->name == "ct"){
            auto *mod = c->module.get();
            c->module.release();

            c->module.reset(new Module(fdn->name, getGlobalContext()));
            auto *recomp = c->compFn(fdn, scope);

            c->jitFunction((Function*)recomp->val);
            c->module.reset(mod);
        }else{
            return c->compErr("Unrecognized compiler directive", vn->loc);
        }

        return fn;
    }else{
        return c->compErr("Unrecognized compiler directive", ppn->loc);
    }
}

TypedValue* Compiler::compFn(FuncDeclNode *fdn, unsigned int scope){
    if(PreProcNode *ppn = dynamic_cast<PreProcNode*>(fdn->modifiers.get())){
        return compPreProcFn(this, fdn, scope, ppn);
    }


    //Get and translate the function's return type to an llvm::Type*
    TypeNode *retNode = (TypeNode*)fdn->type.get();

    //Count the number of parameters
    NamedValNode *paramsBegin = fdn->params.get();
    size_t nParams = getTupleSize(paramsBegin);

    vector<Type*> paramTys = getParamTypes(this, paramsBegin, nParams);

    if(paramTys.size() > 0 && !paramTys.back()){ //varargs fn
        fdn->varargs = true;
        paramTys.pop_back();
    }
    
    if(!retNode)
        return compLetBindingFn(fdn, nParams, paramTys, scope);

    
    //create the function's actual type node for the tval later
    TypeNode *fnTy = createFnTyNode(fdn->params.get(), (TypeNode*)fdn->type.get());


    Type *retTy = typeNodeToLlvmType(retNode);
    FunctionType *ft = FunctionType::get(retTy, paramTys, fdn->varargs);
    Function *f = Function::Create(ft, Function::ExternalLinkage, fdn->name, module.get());
    f->addFnAttr("nounwind");
   
    auto* ret = new TypedValue(f, fnTy);
    stoVar(fdn->name, new Variable(fdn->name, ret, scope));

    //The above handles everything for a function declaration
    //If the function is a definition, then the body will be compiled here.
    if(fdn->child){
        //Create the entry point for the function
        BasicBlock *bb = BasicBlock::Create(getGlobalContext(), "entry", f);
        builder.SetInsertPoint(bb);

        //save the old scope, we will enter the new one provided by the scope param
        unsigned int oldScope = this->scope;

        //set the scope from the callee's scope to the function's scope, variables
        //declared within the function will actually be in one scope higher becuase
        //of the upcoming call to enterNewScope()
        this->scope = varTable.size();

        //tell the compiler to create a new scope on the stack.
        enterNewScope();

        NamedValNode *cParam = paramsBegin;

        //iterate through each parameter and add its value to the new scope.
        for(auto &arg : f->args()){
            TypeNode *paramTyNode = (TypeNode*)cParam->typeExpr.get();
            stoVar(cParam->name, new Variable(cParam->name, new TypedValue(&arg, paramTyNode), this->scope));
            if(!(cParam = (NamedValNode*)cParam->next.get())) break;
        }

        //actually compile the function, and hold onto the last value
        TypedValue *v = fdn->child->compile(this);
        if(!v) return 0;
        
        //End of the function, discard the function's scope.
        exitScope();
   
        this->scope = oldScope;

        //llvm requires explicit returns, so generate a void return even if
        //the user did not in their void function.
        if(retNode && !dynamic_cast<ReturnInst*>(v->val)){
            if(retNode->type == TT_Void){
                builder.CreateRetVoid();
            }else{
                if(*v->type.get() != *retNode){
                    return compErr("Function " + fdn->name + " returned value of type " + 
                            typeNodeToStr(v->type.get()) + " but was declared to return value of type " +
                            typeNodeToStr(retNode), fdn->loc);
                }
                
                if(v->type->type == TT_TaggedUnion)
                    fnTy->extTy->type = TT_TaggedUnion;

                builder.CreateRet(v->val);
            }
        }
        //optimize!
        passManager->run(*f);
    }

    return ret;
}


TypedValue* PreProcNode::compile(Compiler *c){
    return c->getVoidLiteral();
}


string mangle(string& base, TypeNode *paramTys){
    while(paramTys){
        base += "_" + typeNodeToStr(paramTys);
        paramTys = (TypeNode*)paramTys->next.get();
    }
    return base;
}


/*
 *  Registers a function for later compilation
 */
TypedValue* FuncDeclNode::compile(Compiler *c){
    //check if the function is a named function.
    if(name.length() > 0){
        //if it is not, register it to be lazily compiled later (when it is called)
        if((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'){
            name = c->funcPrefix + name;
        }else{
            auto *fnTy = createFnTyNode(this->params.get(), (TypeNode*)this->type.get());

            //sidenote: extTy for function types is guarenteed to be initialized with the return type of
            //the function, so this is not null checked before next is accessed.
            auto *paramTys = (TypeNode*)fnTy->extTy->next.get();
            name = c->funcPrefix + mangle(name, paramTys);
        }

        c->registerFunction(this);
        //and return a void value
        return c->getVoidLiteral();
    }else{
        //Otherwise, if it is a lambda function, compile it now and return it.
        NamedValNode *paramsBegin = this->params.get();
        size_t nParams = Compiler::getTupleSize(paramsBegin);
        vector<Type*> paramTys = getParamTypes(c, paramsBegin, nParams);
        return c->compLetBindingFn(this, nParams, paramTys, c->scope);
    }
}


TypedValue* ExtNode::compile(Compiler *c){
    c->funcPrefix = typeNodeToStr(typeExpr.get()) + "_";
    compileStmtList(methods.get(), c);
    c->funcPrefix = "";
    return c->getVoidLiteral();
}


TypedValue* compTaggedUnion(Compiler *c, DataDeclNode *n){
    vector<string> fieldNames;
    fieldNames.reserve(n->fields);

    auto *nvn = (NamedValNode*)n->child.get();
    vector<string> union_name;
    union_name.push_back(n->name);

    vector<UnionTag*> tags;
    unsigned int largestTyIdx = 0;
    unsigned int largestTySz = 0;
    int i = 0;

    while(nvn){
        TypeNode *tyn = (TypeNode*)nvn->typeExpr.get();
        UnionTag *tag = new UnionTag(nvn->name, deepCopyTypeNode(tyn->extTy.get()), tags.size());

        tags.push_back(tag);

        //Each union member's type is a tuple of the tag, a u8 value, and the user-defined value
        TypeNode *tagTy = deepCopyTypeNode(tyn->extTy.get());

        auto size = tagTy ? tagTy->getSizeInBits(c) : 0;
        if(size > largestTySz){
            largestTySz = size;
            largestTyIdx = i;
        }

        DataType *data = new DataType(union_name, tagTy);
       
        c->stoType(data, nvn->name);

        nvn = (NamedValNode*)nvn->next.get();
        i += 1;
    }

    //use the largest union member's type as the union's type as a whole
    TypeNode *unionTy;

    //check if this is a tagged union, or just a normal enum where the largest contained type is 0 bits
    if(largestTySz == 0){
        unionTy = mkAnonTypeNode(TT_U8);
    }else{
        auto *largestTyn = largestTySz == 0 ? 0 : deepCopyTypeNode(tags[largestTyIdx]->tyn.get());
        unionTy = mkAnonTypeNode(TT_Tuple);

        unionTy->extTy.reset(mkAnonTypeNode(TT_U8));
        unionTy->extTy->next.reset(largestTyn);
    }

    unionTy->typeName = n->name;
    DataType *data = new DataType(fieldNames, unionTy);

    data->tags.swap(tags); 
    c->stoType(data, n->name);
    return c->getVoidLiteral();
}


TypedValue* DataDeclNode::compile(Compiler *c){
    vector<string> fieldNames;
    fieldNames.reserve(fields);

    TypeNode *first = 0;
    TypeNode *nxt = 0;

    auto *nvn = (NamedValNode*)child.get();
    if(((TypeNode*) nvn->typeExpr.get())->type == TT_TaggedUnion){
        return compTaggedUnion(c, this);
    }

    while(nvn){
        TypeNode *tyn = (TypeNode*)nvn->typeExpr.get();

        if(first){
            nxt->next.reset(deepCopyTypeNode(tyn));
            nxt = (TypeNode*)nxt->next.get();
        }else{
            first = deepCopyTypeNode(tyn);
            nxt = first;
        }

        fieldNames.push_back(nvn->name);
        nvn = (NamedValNode*)nvn->next.get();
    }

    DataType *data;
    //check if this is a tuple/function type or a singular type
    if(first->next.get()){
        TypeNode *dataTyn = mkAnonTypeNode(TT_Tuple);
        dataTyn->extTy.reset(first);
        data = new DataType(fieldNames, dataTyn);
    }else{
        data = new DataType(fieldNames, first);
    }

    c->stoType(data, name);
    return c->getVoidLiteral();
}


TypedValue* TraitNode::compile(Compiler *c){
    vector<string> nofields;
    TypeNode *ty = mkAnonTypeNode(TT_Ptr);
    ty->extTy.reset(mkAnonTypeNode(TT_Void));

    DataType *data = new DataType(nofields, ty);
    c->stoType(data, name);

    c->funcPrefix = name + "_";
    compileStmtList(child.get(), c);
    c->funcPrefix = "";
    return c->getVoidLiteral();
}


TypedValue* MatchNode::compile(Compiler *c){
    auto *lval = expr->compile(c);

    if(!lval) return 0;


    if(lval->type->type != TT_TaggedUnion && lval->type->type != TT_Tuple){
        return c->compErr("Cannot match expression of type " + typeNodeToStr(lval->type.get()) + ".  Match expressions must be a tagged union type", expr->loc);
    }


    //the tag is always the zero-th index except for in certain optimization cases and if
    //the tagged union has no tagged values and is equivalent to an enum in C-like languages.
    Value *switchVal = llvmTypeToTypeTag(lval->getType()) == TT_Tuple ?
            c->builder.CreateExtractValue(lval->val, 0)
            : lval->val;

    Function *f = c->builder.GetInsertBlock()->getParent();
    auto *matchbb = c->builder.GetInsertBlock();

    auto *end = BasicBlock::Create(getGlobalContext(), "end_match");
    auto *match = c->builder.CreateSwitch(switchVal, end, branches.size());
    vector<pair<BasicBlock*,TypedValue*>> merges;

    for(auto *mbn : branches){
        ConstantInt *ci = nullptr;
        auto *br = BasicBlock::Create(getGlobalContext(), "br", f);
        c->builder.SetInsertPoint(br);

        //TypeCast-esque pattern:  Maybe n
        if(TypeCastNode *tn = dynamic_cast<TypeCastNode*>(mbn->pattern.get())){
            auto *tagTy = c->lookupType(tn->typeExpr->typeName);
            if(!tagTy)
                return c->compErr("Union tag " + typeNodeToStr(tn->typeExpr.get()) + " was not yet declared.", tn->typeExpr->loc);
       
            if(!tagTy->isUnionTag())
                return c->compErr(typeNodeToStr(tn->typeExpr.get()) + " must be a union tag to be used in a pattern", tn->typeExpr->loc);

            auto *parentTy = c->lookupType(tagTy->getParentUnionName());
            ci = ConstantInt::get(getGlobalContext(), APInt(8, parentTy->getTagVal(tn->typeExpr->typeName), true));

            
            if(VarNode *v = dynamic_cast<VarNode*>(tn->rval.get())){
                auto *alloca = c->builder.CreateAlloca(lval->getType());
                c->builder.CreateStore(lval->val, alloca);

                //cast it from (<tag type>, <largest union member type>) to (<tag type>, <this union member's type>)
                auto *tupTy = StructType::get(getGlobalContext(), {Type::getInt8Ty(getGlobalContext()), c->typeNodeToLlvmType(tagTy->tyn.get())});

                auto *cast = c->builder.CreateBitCast(alloca, tupTy->getPointerTo());
                auto *tup = c->builder.CreateLoad(cast);
                auto *extract = new TypedValue(c->builder.CreateExtractValue(tup, 1), deepCopyTypeNode(tagTy->tyn.get()));
                c->stoVar(v->name, new Variable(v->name, extract, c->scope, true));
            }else{
                return c->compErr("pattern typecast's rval is not a ident", tn->rval->loc);
            }

        //single type pattern:  None
        }else if(TypeNode *tn = dynamic_cast<TypeNode*>(mbn->pattern.get())){
            auto *tagTy = c->lookupType(tn->typeName);
            if(!tagTy)
                return c->compErr("Union tag " + typeNodeToStr(tn) + " was not yet declared.", tn->loc);
       
            if(!tagTy->isUnionTag())
                return c->compErr(typeNodeToStr(tn) + " must be a union tag to be used in a pattern", tn->loc);

            auto *parentTy = c->lookupType(tagTy->getParentUnionName());
            ci = ConstantInt::get(getGlobalContext(), APInt(8, parentTy->getTagVal(tn->typeName), true));

        //variable/match-all pattern: _
        }else if(VarNode *vn = dynamic_cast<VarNode*>(mbn->pattern.get())){
            auto *tn = new TypedValue(lval->val, deepCopyTypeNode(lval->type.get()));
            match->setDefaultDest(br);
            c->stoVar(vn->name, new Variable(vn->name, tn, c->scope, true));
        }else{
            return c->compErr("Pattern matching non-tagged union types is not yet implemented", mbn->pattern->loc);
        }

        auto *then = mbn->branch->compile(c);
        c->builder.CreateBr(end);
        merges.push_back(pair<BasicBlock*,TypedValue*>(c->builder.GetInsertBlock(), then));
      
        //if ci is still null, then the else-like branch was just compiled, and does not correlate to a singular tag
        if(ci)
            match->addCase(ci, br);
    }

    f->getBasicBlockList().push_back(end);
    c->builder.SetInsertPoint(end);

    if(!merges[0].second) return 0;

    if(merges[0].second->type->type != TT_Void){
        auto *phi = c->builder.CreatePHI(merges[0].second->getType(), branches.size());
        for(auto &pair : merges)
            if(!dynamic_cast<ReturnInst*>(pair.second->val))
                phi->addIncoming(pair.second->val, pair.first);

        phi->addIncoming(UndefValue::get(merges[0].second->getType()), matchbb);
        return new TypedValue(phi, deepCopyTypeNode(merges[0].second->type.get()));
    }else{
        return c->getVoidLiteral();
    }
}

TypedValue* MatchBranchNode::compile(Compiler *c){
    return c->getVoidLiteral();
}


TypedValue* Compiler::getFunction(string& name){
    auto *f = lookup(name);
    if(!f){
        if(auto *pair = fnDecls[name]){
            //Function has been declared but not defined, so define it.
            BasicBlock *caller = builder.GetInsertBlock();
            auto *fn = compFn(pair->fdn, pair->scope);
            fnDecls.erase(name);
            builder.SetInsertPoint(caller);
            return fn;
        }
        return 0;
    }else{
        return f->tval;
    }
}
    
TypedValue* Compiler::getMangledFunction(string name, TypeNode *params){
    string fnName = mangle(name, params);
    return getFunction(fnName);
}

/*
 * imports a given ante file to the current module
 * inputted file must exist and be a valid ante source file.
 */
void Compiler::importFile(const char *fName){
    Compiler *c = new Compiler(fName, true);
    c->scanAllDecls();

    if(c->errFlag){
        cout << "Error when importing " << fName << endl;
        errFlag = true;
        delete c;
        return;
    }

    //copy import's userTypes into importer
    for(const auto& it : c->userTypes){
        userTypes[it.first] = it.second;
    }

    //copy functions, but change their scope first
    for(const auto& it : c->fnDecls){
        it.second->scope = this->scope;
        fnDecls[it.first] = it.second;
    }

    delete c;
}


TypeNode* mkAnonTypeNode(TypeTag t){
    auto* empty = new string("");
    
    auto fakeLoc = yy::location(yy::position(empty, 0, 0),
                                yy::position(empty, 0, 0));
    
    return new TypeNode(fakeLoc, t, "", nullptr);
}


/*
 *  Declares functions to be included in every module without need of an import.
 *  These are registered but not compiled until they are called so that they
 *  do not pollute the module with unused definitions.
 */
void Compiler::compilePrelude(){
    if(fileName != LIB_DIR "/prelude.an")
        importFile(LIB_DIR "/prelude.an");
}


/*
 *  Removes .an from a source file to get its module name
 */
string removeFileExt(string file){
    auto index = file.find('.');
    return index == string::npos ? file : file.substr(0, index);
}


/*
 *  Adds a function to the list of declared, but not defined functions.  A declared function's
 *  FuncDeclNode can be added to be compiled only when it is later called.  Useful to prevent pollution
 *  of a module with unneeded library functions.
 */
inline void Compiler::registerFunction(FuncDeclNode *fn){
    fnDecls[fn->name] = new FuncDecl(fn, this->scope);
}

/*
 *  Sweeps through entire parse tree registering all function and data
 *  declarations.  Removes compiled functions.
 */
void Compiler::scanAllDecls(){
    Node *op = ast.get();
    BinOpNode *prev = 0;
    BinOpNode *bop;

    while((bop = dynamic_cast<BinOpNode*>(op)) && bop->op == ';'){
        auto *rv = bop->rval.get();

        if(dynamic_cast<FuncDeclNode*>(rv) || dynamic_cast<ExtNode*>(rv) || dynamic_cast<DataDeclNode*>(rv)){
            rv->compile(this); //register the function
            if(prev){
                prev->lval.release();
                prev->lval.reset(bop->lval.get()); //free the node
            }else{
                ast.release();
                ast.reset(bop->lval.get()); //free the node
            }
            op = bop->lval.get();
            bop->rval.release();
            bop->lval.release();
            delete bop;
        }else{
            prev = bop;
            op = bop->lval.get();
        }
    }

    //check the final node
    if(dynamic_cast<FuncDeclNode*>(op) || dynamic_cast<ExtNode*>(op) || dynamic_cast<DataDeclNode*>(op)){
        op->compile(this); //register the function`
        if(prev){
            prev->lval.release();
            prev->lval.reset(mkAnonTypeNode(TT_Void)); //TODO: replace this node with one that does not rely on the ::compile function being empty
        }else{
            ast.release();
            ast.reset(mkAnonTypeNode(TT_Void)); //TODO: replace this node with one that does not rely on the ::compile function being empty
        }
    }
}

//evaluates and prints a single-expression module
//Used in REPL
void Compiler::eval(){
    auto *tval = ast->compile(this);
    tval->val->dump();
}

void Compiler::compile(){
    //get or create the function type for the main method: void()
    FunctionType *ft = FunctionType::get(Type::getInt8Ty(getGlobalContext()), false);
    
    //Actually create the function in module m
    string fnName = isLib ? "init_" + removeFileExt(fileName) : "main";
    Function *main = Function::Create(ft, Function::ExternalLinkage, fnName, module.get());

    //Create the entry point for the function
    BasicBlock *bb = BasicBlock::Create(getGlobalContext(), "entry", main);
    builder.SetInsertPoint(bb);
    
    compilePrelude();
    scanAllDecls();

    //Compile the rest of the program
    ast->compile(this);
    exitScope();

    //builder should already be at end of main function
    builder.CreateRet(ConstantInt::get(getGlobalContext(), APInt(8, 0, true)));
    
    passManager->run(*main);


    //flag this module as compiled.
    compiled = true;

    if(errFlag){
        puts("Compilation aborted.");
        exit(1);
    }
}


void Compiler::compileNative(){
    if(!compiled) compile();

    string modName = removeFileExt(fileName);
    //this file will become the obj file before linking
    string objFile = modName + ".o";

    if(!compileIRtoObj(objFile)){
        linkObj(objFile, modName);
        remove(objFile.c_str());
    }
}

//returns 0 on success
int Compiler::compileObj(){
    if(!compiled) compile();

    string modName = removeFileExt(fileName);
    string objFile = modName + ".o";

    return compileIRtoObj(objFile);
}


const Target* getTarget(){
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    string err = "";

    string triple = Triple(AN_NATIVE_ARCH, AN_NATIVE_VENDOR, AN_NATIVE_OS).getTriple();
    const Target* target = TargetRegistry::lookupTarget(triple, err);

    if(!err.empty()){
        cerr << err << endl;
        exit(1);
    }

    return target;
}

TargetMachine* getTargetMachine(){
    auto *target = getTarget();

    string cpu = "";
    string features = "";
    string triple = Triple(AN_NATIVE_ARCH, AN_NATIVE_VENDOR, AN_NATIVE_OS).getTriple();
    TargetOptions op;
    
    TargetMachine *tm = target->createTargetMachine(triple, cpu, features, op, Reloc::Model::Default, 
            CodeModel::Default, CodeGenOpt::Level::Aggressive);

    if(!tm){
        cerr << "Error when initializing TargetMachine.\n";
        exit(1);
    }
    
    return tm;
}
void Compiler::jitFunction(Function *f){
    if(!jit.get()){
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
        auto* eBuilder = new EngineBuilder(unique_ptr<Module>(module.get()));

        string err;

        jit.reset(eBuilder->setErrorStr(&err).setEngineKind(EngineKind::JIT).create());
        if(err.length() > 0) cerr << err << endl;
    }

    compileIRtoObj((".tmp_" + f->getName()).str());
    jit->addModule(move(module));
    jit->finalizeObject();
    remove((".tmp_" + f->getName()).str().c_str());
    
    auto* fn = jit->getPointerToFunction(f);

    if(fn)
        reinterpret_cast<void(*)()>(fn)();
}

void Compiler::jitFunction(string& fnName){
    if(!jit.get()){
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
        auto* eBuilder = new EngineBuilder(unique_ptr<Module>(module.get()));

        string err;

        jit.reset(eBuilder->setErrorStr(&err).setEngineKind(EngineKind::JIT).create());
        if(err.length() > 0) cerr << err << endl;
    }

    compileIRtoObj(".tmp_" + fnName);
    jit->addModule(move(module));
    jit->finalizeObject();
    remove((".tmp_" + fnName).c_str());
    
    auto* fn = jit->getPointerToNamedFunction("testfn");

    if(fn)
        reinterpret_cast<void(*)()>(fn)();
}

/*
 *  Compiles a module into a .o file to be used for linking.
 *  Invokes llc.
 */
int Compiler::compileIRtoObj(string outFile){
    auto *tm = getTargetMachine();

    std::error_code errCode;
    raw_fd_ostream out{outFile, errCode, sys::fs::OpenFlags::F_RW};

    legacy::PassManager pm;
    int res = tm->addPassesToEmitFile(pm, out, llvm::TargetMachine::CGFT_ObjectFile);
    pm.run(*module);

    return res;
}


int Compiler::linkObj(string inFiles, string outFile){
    //invoke gcc to link the module.
    string cmd = "gcc " + inFiles + " -o " + outFile;
    return system(cmd.c_str());
}


/*
 *  Dumps current contents of module to stdout
 */
void Compiler::emitIR(){
    if(!compiled) compile();
    if(errFlag) puts("Partially compiled module: \n");
    module->dump();
}


inline void Compiler::enterNewScope(){
    scope++;
    auto *vtable = new map<string, Variable*>();
    varTable.push_back(unique_ptr<map<string, Variable*>>(vtable));
}


inline void Compiler::exitScope(){
    //iterate through all known variables, check for pointers at the end of
    //their lifetime, and insert calls to free for any that are found
    auto vtable = varTable.back().get();

    for(auto it = vtable->cbegin(); it != vtable->cend(); it++){
        if(it->second->isFreeable() && it->second->scope == this->scope){
            string freeFnName = "free";
            Function* freeFn = (Function*)getFunction(freeFnName)->val;

            auto *inst = dynamic_cast<AllocaInst*>(it->second->getVal());
            auto *val = inst? builder.CreateLoad(inst) : it->second->getVal();

            //change the pointer's type to void so it is not freed again
            it->second->tval->type->type = TT_Void;

            //cast the freed value to i32* as that is what free accepts
            Type *vPtr = freeFn->getFunctionType()->getFunctionParamType(0);
            val = builder.CreatePointerCast(val, vPtr);
            builder.CreateCall(freeFn, val);
        }
    }

    scope--;
    varTable.pop_back();
}


Variable* Compiler::lookup(string var) const{
    for(auto it = varTable.crbegin(); it != varTable.crend(); ++it){
        try{
            return (*it)->at(var);
        }catch(out_of_range r){}
    }
    return nullptr;
}


void Compiler::stoVar(string var, Variable *val){
    (*varTable[val->scope-1])[var] = val;
}


DataType* Compiler::lookupType(string tyname) const{
    try{
        return userTypes.at(tyname);
    }catch(out_of_range r){
        return nullptr;
    }
}


inline void Compiler::stoType(DataType *ty, string &typeName){
    userTypes[typeName] = ty;
}

Compiler::Compiler(const char *_fileName, bool lib) :
        builder(getGlobalContext()), 
        errFlag(false),
        compiled(false),
        isLib(lib),
        fileName(_fileName? _fileName : "(stdin)"),
        funcPrefix(""){

    setLexer(new Lexer(_fileName));
    yy::parser p{};
    int flag = p.parse();
    if(flag != PE_OK){ //parsing error, cannot procede
        //print out remaining errors
        int tok;
        yy::location loc;
        loc.initialize();
        while((tok = yylexer->next(&loc)) != Tok_Newline && tok != 0);
        while(p.parse() != PE_OK && yylexer->peek() != 0);

        fputs("Syntax error, aborting.\n", stderr);
        exit(flag);
    }

    scope = 0;
    enterNewScope();

    ast.reset(parser::getRootNode());
    module.reset(new Module(removeFileExt(fileName.c_str()), getGlobalContext()));

    //add passes to passmanager.
    //TODO: change passes based on -O0 through -O3 flags
    passManager.reset(new legacy::FunctionPassManager(module.get()));
    //passManager->add(createBasicAliasAnalysisPass());
    passManager->add(createGVNPass());
    passManager->add(createCFGSimplificationPass());
    passManager->add(createTailCallEliminationPass());
    passManager->add(createPromoteMemoryToRegisterPass());
    passManager->add(createInstructionCombiningPass());
    passManager->add(createReassociatePass());
    passManager->doInitialization();
}

Compiler::~Compiler(){
    fnDecls.clear();
    if(yylexer){
        delete yylexer;
        yylexer = nullptr;
    }
}
