// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler.h"
#include "vm/dil_to_il.h"
#include "vm/intermediate_language.h"
#include "vm/stack_frame.h"

namespace dart {
namespace dil {

#define Z (zone_)
#define H (translation_helper_)

Fragment& Fragment::operator+=(const Fragment& other) {
  if (entry == NULL) {
    entry = other.entry;
    current = other.current;
  } else if (current != NULL && other.entry != NULL) {
    current->LinkTo(other.entry);
    current = other.current;
  }
  return *this;
}


Fragment& Fragment::operator<<=(Instruction* next) {
  if (entry == NULL) {
    entry = current = next;
  } else if (current != NULL) {
    current->LinkTo(next);
    current = next;
  }
  return *this;
}


Fragment Fragment::closed() {
  ASSERT(entry != NULL);
  return Fragment(entry, NULL);
}


Fragment operator+(const Fragment& first, const Fragment& second) {
  Fragment result = first;
  result += second;
  return result;
}


Fragment operator<<(const Fragment& fragment, Instruction* next) {
  Fragment result = fragment;
  result <<= next;
  return result;
}


const dart::String& TranslationHelper::DartString(const char* content) {
  return dart::String::ZoneHandle(Z, dart::String::New(content));
}


dart::String& TranslationHelper::DartString(String* content,
                                            Heap::Space space) {
  return dart::String::ZoneHandle(Z,
      dart::String::FromUTF8(content->buffer(), content->size(), space));
}


const dart::String& TranslationHelper::DartSymbol(const char* content) {
  return dart::String::ZoneHandle(Z, Symbols::New(content));
}


const dart::String& TranslationHelper::DartSymbol(String* content) {
  return dart::String::ZoneHandle(Z,
      dart::Symbols::FromUTF8(content->buffer(), content->size()));
}


const dart::String& TranslationHelper::DartClassName(dil::Class* dil_klass) {
  if (dil_klass->name() != NULL) {
    ASSERT(dil_klass->IsNormalClass());
    return DartSymbol(dil_klass->name());
  } else {
    ASSERT(dil_klass->IsMixinClass());

    // We construct the string from right to left:
    //     "Base&Mixin1&Mixin2&...&MixinN"
    dart::String& partial = dart::String::Handle(Z, dart::String::New(""));
    dart::String& amp = dart::String::Handle(Z, dart::String::New("&"));
    dart::String& tmp = dart::String::Handle(Z);
    while (dil_klass->name() == NULL) {
      ASSERT(dil_klass->IsMixinClass());

      MixinClass* dil_mixin_class = MixinClass::Cast(dil_klass);
      InterfaceType* base_type = dil_mixin_class->first();
      InterfaceType* mixin_type = dil_mixin_class->second();

      String* mixin_name = NormalClass::Cast(mixin_type->klass())->name();

      tmp ^= dart::String::FromUTF8(mixin_name->buffer(), mixin_name->size());

      partial ^= dart::String::Concat(amp, partial);
      partial ^= dart::String::Concat(tmp, partial);

      dil_klass = base_type->klass();
    }

    tmp ^= dart::String::FromUTF8(
        dil_klass->name()->buffer(), dil_klass->name()->size());

    partial ^= dart::String::Concat(amp, partial);
    partial ^= dart::String::Concat(tmp, partial);

    partial ^= dart::Symbols::New(partial);
    return partial;
  }
}


const dart::String& TranslationHelper::DartConstructorName(Constructor* node) {
  Class* klass = Class::Cast(node->parent());

  // We build a String which looks like <classname>.<constructor-name>.
  dart::String& temp = DartString(klass->name());
  temp ^= dart::String::Concat(temp, Symbols::Dot());
  temp ^= dart::String::Concat(
      temp, DartString(node->name()->string()));  // NOLINT
  return dart::String::ZoneHandle(Z, dart::Symbols::New(temp));
}


const dart::String& TranslationHelper::DartProcedureName(Procedure* procedure) {
  if (procedure->kind() == Procedure::kSetter) {
    return DartSetterName(procedure->name()->string());
  } else if (procedure->kind() == Procedure::kGetter) {
    return DartGetterName(procedure->name()->string());
  } else {
    return DartSymbol(procedure->name()->string());
  }
}


const dart::String& TranslationHelper::DartSetterName(String* content) {
  // The names flowing into [content] are coming from the DILL file:
  //   * user-defined setters: `fieldname=`
  //   * property-set expressions:  `fieldname`
  //
  // The VM uses `get:fieldname` and `set:fieldname`.
  //
  // => In order to be consistent, we remove the `=` always and adopt the VM
  //    conventions.
  ASSERT(content->size() > 0);
  int skip = 0;
  if (content->buffer()[content->size() - 1] == '=') {
    skip = 1;
  }
  dart::String& name = dart::String::ZoneHandle(Z,
      dart::String::FromUTF8(content->buffer(), content->size() - skip));
  return dart::String::ZoneHandle(Z, dart::Field::SetterSymbol(name));
}


const dart::String& TranslationHelper::DartGetterName(String* content) {
  return dart::String::ZoneHandle(Z,
      dart::Field::GetterSymbol(DartString(content)));
}


FlowGraphBuilder::FlowGraphBuilder(TreeNode* node,
                                   ParsedFunction* parsed_function,
                                   int first_block_id)
  : zone_(Thread::Current()->zone()),
    translation_helper_(zone_),
    node_(node),
    parsed_function_(parsed_function),
    library_(dart::Library::ZoneHandle(Z,
        dart::Class::Handle(parsed_function->function().Owner()).library())),
    ic_data_array_(Z, 0),
    next_block_id_(first_block_id),
    scope_(NULL),
    loop_depth_(0),
    stack_(NULL),
    pending_argument_count_(0),
    this_variable_(NULL) {
}


Fragment FlowGraphBuilder::AllocateObject(const dart::Class& klass) {
  ArgumentArray arguments = new(Z) ZoneGrowableArray<PushArgumentInstr*>(Z, 0);
  AllocateObjectInstr* allocate =
      new(Z) AllocateObjectInstr(TokenPosition::kNoSource, klass, arguments);
  Push(allocate);
  return Fragment(allocate);
}


Fragment FlowGraphBuilder::BooleanNegate() {
  BooleanNegateInstr* negate = new(Z) BooleanNegateInstr(Pop());
  Push(negate);
  return Fragment(negate);
}


Fragment FlowGraphBuilder::Boolify() {
  Fragment instructions = Constant(Bool::True());
  Value* constant_value = Pop();
  StrictCompareInstr* compare =
      new(Z) StrictCompareInstr(TokenPosition::kNoSource,
                                Token::kEQ_STRICT,
                                Pop(),
                                constant_value,
                                false);
  Push(compare);
  return instructions << compare;
}


Fragment FlowGraphBuilder::Branch(TargetEntryInstr** then_entry,
                                  TargetEntryInstr** otherwise_entry) {
  Fragment instructions = Constant(Bool::True());
  Value* constant_value = Pop();
  StrictCompareInstr* compare =
      new(Z) StrictCompareInstr(TokenPosition::kNoSource,
                                Token::kEQ_STRICT,
                                Pop(),
                                constant_value,
                                false);
  BranchInstr* branch = new(Z) BranchInstr(compare);
  *then_entry = *branch->true_successor_address() =
      new(Z) TargetEntryInstr(AllocateBlockId(),
                              CatchClauseNode::kInvalidTryIndex);
  *otherwise_entry = *branch->false_successor_address() =
      new(Z) TargetEntryInstr(AllocateBlockId(),
                              CatchClauseNode::kInvalidTryIndex);
  return (instructions << branch).closed();
}


Fragment FlowGraphBuilder::CheckStackOverflow() {
  return Fragment(new(Z) CheckStackOverflowInstr(TokenPosition::kNoSource,
                                                 loop_depth_));
}


Fragment FlowGraphBuilder::Constant(const Object& value) {
  ConstantInstr* constant = new(Z) ConstantInstr(value);
  Push(constant);
  return Fragment(constant);
}


Fragment FlowGraphBuilder::Goto(JoinEntryInstr* destination) {
  return Fragment(new(Z) GotoInstr(destination)).closed();
}


Fragment FlowGraphBuilder::InstanceCall(const dart::String& name,
                                        Token::Kind kind,
                                        int argument_count) {
  return InstanceCall(name, kind, argument_count, Array::null_array());
}


Fragment FlowGraphBuilder::InstanceCall(const dart::String& name,
                                        Token::Kind kind,
                                        int argument_count,
                                        const Array& argument_names) {
  ArgumentArray arguments = GetArguments(argument_count);
  InstanceCallInstr* call =
      new(Z) InstanceCallInstr(TokenPosition::kNoSource,
          name,
          kind,
          arguments,
          argument_names,
          1,
          ic_data_array_);
  Push(call);
  return Fragment(call);
}


Fragment FlowGraphBuilder::LoadField(const dart::Field& field) {
  LoadFieldInstr* load =
      new(Z) LoadFieldInstr(Pop(),
                            &field,
                            AbstractType::ZoneHandle(Z, field.type()),
                            TokenPosition::kNoSource);
  Push(load);
  return Fragment(load);
}


Fragment FlowGraphBuilder::LoadLocal(LocalVariable* variable) {
  LoadLocalInstr* load =
      new(Z) LoadLocalInstr(*variable, TokenPosition::kNoSource);
  Push(load);
  return Fragment(load);
}


Fragment FlowGraphBuilder::LoadStaticField() {
  LoadStaticFieldInstr* load =
      new(Z) LoadStaticFieldInstr(Pop(), TokenPosition::kNoSource);
  Push(load);
  return Fragment(load);
}


Fragment FlowGraphBuilder::NullConstant() {
  return Constant(Instance::ZoneHandle(Z, Instance::null()));
}


Fragment FlowGraphBuilder::PushArgument() {
  PushArgumentInstr* argument = new(Z) PushArgumentInstr(Pop());
  Push(argument);

  argument->set_temp_index(argument->temp_index() - 1);
  ++pending_argument_count_;

  return Fragment(argument);
}


Fragment FlowGraphBuilder::Return() {
  Value* value = Pop();
  ASSERT(stack_ == NULL);
  return Fragment(new(Z) ReturnInstr(TokenPosition::kNoSource, value)).closed();
}


Fragment FlowGraphBuilder::StaticCall(const Function& target,
                                      int argument_count) {
  return StaticCall(target, argument_count, Array::null_array());
}


Fragment FlowGraphBuilder::StaticCall(const Function& target,
                                      int argument_count,
                                      const Array& argument_names) {
  ArgumentArray arguments = GetArguments(argument_count);
  StaticCallInstr* call =
      new(Z) StaticCallInstr(TokenPosition::kNoSource,
                             target,
                             argument_names,
                             arguments,
                             ic_data_array_);
  Push(call);
  return Fragment(call);
}


Fragment FlowGraphBuilder::StoreInstanceField(const dart::Field& field) {
  Value* value = Pop();
  StoreInstanceFieldInstr* store =
      new(Z) StoreInstanceFieldInstr(field,
                                     Pop(),
                                     value,
                                     kEmitStoreBarrier,
                                     TokenPosition::kNoSource);
  return Fragment(store);
}


Fragment FlowGraphBuilder::StoreLocal(LocalVariable* variable) {
  StoreLocalInstr* store =
      new(Z) StoreLocalInstr(*variable, Pop(), TokenPosition::kNoSource);
  Push(store);
  return Fragment(store);
}


Fragment FlowGraphBuilder::StoreStaticField(const dart::Field& field) {
  return Fragment(
      new(Z) StoreStaticFieldInstr(field, Pop(), TokenPosition::kNoSource));
}


dart::RawLibrary* FlowGraphBuilder::LookupLibraryByDilLibrary(
    Library* dil_library) {
  const dart::String& library_name = H.DartSymbol(dil_library->import_uri());
  ASSERT(!library_name.IsNull());
  dart::RawLibrary* library = dart::Library::LookupLibrary(library_name);
  ASSERT(library != Object::null());
  return library;
}


dart::RawClass* FlowGraphBuilder::LookupClassByName(const dart::String& name) {
  dart::RawClass* klass = library_.LookupClassAllowPrivate(name);
  ASSERT(klass != Object::null());
  return klass;
}


dart::RawClass* FlowGraphBuilder::LookupClassByName(String* name) {
  dart::RawClass* klass = LookupClassByName(H.DartString(name));
  ASSERT(klass != Object::null());
  return klass;
}


dart::RawClass* FlowGraphBuilder::LookupClassByDilClass(Class* dil_klass) {
  dart::RawClass* klass = NULL;

  const dart::String& class_name = H.DartString(dil_klass->name());
  Library* dil_library = Library::Cast(dil_klass->parent());
  dart::Library& library = dart::Library::Handle(
      LookupLibraryByDilLibrary(dil_library));
  klass = library.LookupClassAllowPrivate(class_name);

  ASSERT(klass != Object::null());
  return klass;
}


dart::RawField* FlowGraphBuilder::LookupFieldByName(const dart::String& name) {
  dart::RawField* field = library_.LookupFieldAllowPrivate(name);
  ASSERT(field != Object::null());
  return field;
}


dart::RawField* FlowGraphBuilder::LookupFieldByName(String* name) {
  dart::RawField* field = LookupFieldByName(H.DartString(name));
  ASSERT(field != Object::null());
  return field;
}


dart::RawField* FlowGraphBuilder::LookupFieldByDilField(Field* dil_field) {
  TreeNode* node = dil_field->parent();

  dart::Class& klass = dart::Class::Handle(Z);
  if (node->IsClass()) {
    klass ^= LookupClassByDilClass(Class::Cast(node));
  } else {
    ASSERT(node->IsLibrary());
    dart::Library& library = dart::Library::Handle(
        Z, LookupLibraryByDilLibrary(Library::Cast(node)));
    klass = library.toplevel_class();
  }
  dart::RawField* field = klass.LookupFieldAllowPrivate(
      H.DartSymbol(dil_field->name()->string()));
  ASSERT(field != Object::null());
  return field;
}


dart::RawFunction* FlowGraphBuilder::LookupStaticMethodByDilProcedure(
    Procedure* procedure) {
  ASSERT(procedure->IsStatic());
  const dart::String& procedure_name = H.DartProcedureName(procedure);

  // The parent is either a library or a class (in which case the procedure is a
  // static method).
  TreeNode* parent = procedure->parent();
  if (parent->IsClass()) {
    dart::Class& klass = dart::Class::Handle(Z,
        LookupClassByDilClass(Class::Cast(parent)));
    dart::RawFunction* function =
        klass.LookupFunctionAllowPrivate(procedure_name);
    ASSERT(function != Object::null());
    return function;
  } else {
    ASSERT(parent->IsLibrary());
    dart::Library& library = dart::Library::Handle(
        LookupLibraryByDilLibrary(Library::Cast(parent)));
    dart::RawFunction* function =
        library.LookupFunctionAllowPrivate(procedure_name);
    ASSERT(function != Object::null());
    return function;
  }
}


dart::RawFunction* FlowGraphBuilder::LookupStaticMethodByName(
    const dart::String& name) {
  dart::RawFunction* function = library_.LookupFunctionAllowPrivate(name);
  ASSERT(function != Object::null());
  return function;
}


dart::RawFunction* FlowGraphBuilder::LookupStaticMethodByName(String* name) {
  dart::RawFunction* function = LookupStaticMethodByName(
      H.DartString(name));
  ASSERT(function != Object::null());
  return function;
}


dart::RawFunction* FlowGraphBuilder::LookupConstructorByDilConstructor(
    const dart::Class& owner, Constructor* constructor) {
  dart::RawFunction* function =
      owner.LookupConstructorAllowPrivate(H.DartConstructorName(constructor));
  ASSERT(function != Object::null());
  return function;
}


LocalVariable* FlowGraphBuilder::MakeTemporary() {
  char name[64];
  intptr_t index = stack_->definition()->temp_index();
  OS::SNPrint(name, 64, ":temp%" Pd, index);
  LocalVariable* variable =
      new(Z) LocalVariable(TokenPosition::kNoSource,
                           H.DartSymbol(name),
                           *stack_->Type()->ToAbstractType());
  // Set the index relative to the base of the expression stack including
  // outgoing arguments.  Later this will be adjusted to be relative to the
  // frame pointer.
  variable->set_index(-(index + pending_argument_count_));
  temporaries_.push_back(variable);

  // The value has uses as if it were a local variable.  Mark the definition
  // as used so that its temp index will not be cleared (causing it to never
  // be materialized in the expression stack).
  stack_->definition()->set_ssa_temp_index(0);

  return variable;
}


void FlowGraphBuilder::AddVariable(VariableDeclaration* declaration,
                                   LocalVariable* variable) {
  ASSERT(variable != NULL);
  scope_->AddVariable(variable);
  locals_[declaration] = variable;
}


void FlowGraphBuilder::AddParameter(VariableDeclaration* declaration,
                                    LocalVariable* variable,
                                    int pos) {
  ASSERT(variable != NULL);
  scope_->InsertParameterAt(pos, variable);
  locals_[declaration] = variable;
}


dart::LocalVariable* FlowGraphBuilder::LookupVariable(
    VariableDeclaration* var) {
  LocalVariable* local = locals_[var];
  ASSERT(local != NULL);
  return local;
}


void FlowGraphBuilder::SetTempIndex(Definition* definition) {
  definition->set_temp_index(
      stack_ == NULL ? 0 : stack_->definition()->temp_index() + 1);
}


void FlowGraphBuilder::Push(Definition* definition) {
  SetTempIndex(definition);
  Value::AddToList(new(Z) Value(definition), &stack_);
}


Value* FlowGraphBuilder::Pop() {
  ASSERT(stack_ != NULL);
  Value* value = stack_;
  stack_ = value->next_use();
  if (stack_ != NULL) stack_->set_previous_use(NULL);

  value->set_next_use(NULL);
  value->set_previous_use(NULL);
  value->definition()->ClearSSATempIndex();
  return value;
}


Fragment FlowGraphBuilder::Drop() {
  ASSERT(stack_ != NULL);
  Fragment instructions;
  Definition* definition = stack_->definition();
  if (definition->HasSSATemp()) {
    instructions <<= new(Z) DropTempsInstr(1, NULL);
  } else {
    definition->ClearTempIndex();
  }

  Pop();
  return instructions;
}


FlowGraph* FlowGraphBuilder::BuildGraph() {
  const dart::Function& function = parsed_function_->function();

  if (function.IsImplicitClosureFunction()) return NULL;
  if (function.IsConstructorClosureFunction()) return NULL;

  // The IR builder will create its own local variables and scopes, and it
  // will not need an AST.  The code generator will assume that there is a
  // local variable stack slot allocated for the current context and (I
  // think) that the runtime will expect it to be at a fixed offset which
  // requires allocating an unused expression temporary variable.
  scope_ = new LocalScope(NULL, 0, 0);
  scope_->AddVariable(parsed_function_->EnsureExpressionTemp());
  scope_->AddVariable(parsed_function_->current_context_var());
  parsed_function_->SetNodeSequence(
      new SequenceNode(TokenPosition::kNoSource, scope_));

  switch (function.kind()) {
    case RawFunction::kClosureFunction:
    case RawFunction::kRegularFunction:
    case RawFunction::kGetterFunction:
    case RawFunction::kSetterFunction:
    case RawFunction::kConstructor: {
      if (node_->IsProcedure()) {
        return BuildGraphOfFunction(Procedure::Cast(node_)->function());
      } else if (node_->IsConstructor()) {
        return BuildGraphOfFunction(Constructor::Cast(node_)->function());
      } else {
        UNIMPLEMENTED();
      }
    }
    case RawFunction::kImplicitGetter:
    case RawFunction::kImplicitSetter: {
      ASSERT(node_->IsField());
      return BuildGraphOfFieldAccessor(Field::Cast(node_));
    }
    default: {
      UNREACHABLE();
      return NULL;
    }
  }
}


FlowGraph* FlowGraphBuilder::BuildGraphOfFunction(FunctionNode* function) {
  TargetEntryInstr* normal_entry =
      new(Z) TargetEntryInstr(AllocateBlockId(),
                              CatchClauseNode::kInvalidTryIndex);
  GraphEntryInstr* graph_entry =
      new(Z) GraphEntryInstr(*parsed_function_, normal_entry,
                             Compiler::kNoOSRDeoptId);

  // Populate stack with arguments
  {
    bool is_method = !parsed_function_->function().IsStaticFunction();

    dart::AbstractType& dynamic =
        dart::AbstractType::ZoneHandle(Type::DynamicType());
    int pos = 0;
    if (is_method) {
      dart::Class& klass =
          dart::Class::Handle(parsed_function_->function().Owner());
      Type& klass_type =
          Type::ZoneHandle(Type::NewNonParameterizedType(klass));
      this_variable_ = new(Z) LocalVariable(
          TokenPosition::kNoSource,
          H.DartSymbol("this"),
          klass_type);
      AddParameter(NULL, this_variable_, pos);
      pos++;
    }
    for (int i = 0;
         i < function->positional_parameters().length();
         i++, pos++) {
      VariableDeclaration* var = function->positional_parameters()[i];
      LocalVariable* parameter = new(Z) LocalVariable(
          TokenPosition::kNoSource,
          H.DartSymbol(var->name()),
          dynamic);
      AddParameter(var, parameter, pos);
    }
    for (int i = 0; i < function->named_parameters().length(); i++, pos++) {
      VariableDeclaration* var = function->named_parameters()[i];
      LocalVariable* parameter = new(Z) LocalVariable(
          TokenPosition::kNoSource,
          H.DartSymbol(var->name()),
          dynamic);
      AddParameter(var, parameter, pos);
    }
  }

  // Setup default values for optional parameters.
  int num_optional_parameters =
      parsed_function_->function().NumOptionalParameters();
  if (num_optional_parameters > 0) {
    ZoneGrowableArray<const Instance*>* default_values =
        new ZoneGrowableArray<const Instance*>(Z, num_optional_parameters);

    if (parsed_function_->function().HasOptionalNamedParameters()) {
      ASSERT(!parsed_function_->function().HasOptionalPositionalParameters());
      for (int i = 0; i < num_optional_parameters; i++) {
        // FIXME(kustermann):
        // We should evaluate the constant expression:
        //
        //     VariableDeclaration* variable =
        //         procedure_->function()->named_parameters()[i];
        //     default_value =
        //         EvaluateConstantExpression(variable->initializer());
        const Instance* default_value = &Instance::ZoneHandle(Instance::null());
        default_values->Add(default_value);
      }
    } else {
      ASSERT(parsed_function_->function().HasOptionalPositionalParameters());
      // int required = procedure_->function()->required_parameter_count();
      for (int i = 0; i < num_optional_parameters; i++) {
        // FIXME(kustermann):
        // We should evaluate the constant expression:
        //
        //     VariableDeclaration* variable =
        //         procedure_->function()->positional_parameters()[i];
        //     default_value =
        //         EvaluateConstantExpression(variable->initializer());
        const Instance* default_value = &Instance::ZoneHandle(Instance::null());
        default_values->Add(default_value);
      }
    }
    parsed_function_->set_default_parameter_values(default_values);
  }

  Fragment body(normal_entry);
  body += CheckStackOverflow();
  body += TranslateStatement(function->body());

  if (body.is_open()) {
    body += NullConstant();
    body += Return();
  }

  return new(Z) FlowGraph(*parsed_function_, graph_entry, next_block_id_ - 1);
}


FlowGraph* FlowGraphBuilder::BuildGraphOfFieldAccessor(Field* dil_field) {
  bool is_setter = parsed_function_->function().IsImplicitSetterFunction();
  dart::Field& field =
      dart::Field::ZoneHandle(Z, LookupFieldByDilField(dil_field));

  TargetEntryInstr* normal_entry =
      new(Z) TargetEntryInstr(AllocateBlockId(),
                              CatchClauseNode::kInvalidTryIndex);
  GraphEntryInstr* graph_entry =
      new(Z) GraphEntryInstr(*parsed_function_, normal_entry,
                             Compiler::kNoOSRDeoptId);

  // Populate stack with arguments.
  LocalVariable* setter_value = NULL;
  {
    dart::AbstractType& dynamic =
        dart::AbstractType::ZoneHandle(Type::DynamicType());
    dart::Class& klass =
        dart::Class::Handle(parsed_function_->function().Owner());
    Type& klass_type =
        Type::ZoneHandle(Type::NewNonParameterizedType(klass));

    this_variable_ = new(Z) LocalVariable(
        TokenPosition::kNoSource,
        dart::String::ZoneHandle(Symbols::New("this")),
        klass_type);
    scope_->InsertParameterAt(0, this_variable_);

    if (is_setter) {
      setter_value = new(Z) LocalVariable(
          TokenPosition::kNoSource,
          H.DartSymbol("value"),
          dynamic);
      scope_->InsertParameterAt(1, setter_value);
    }
  }

  // Generate getter or setter body
  Fragment body(normal_entry);
  if (!is_setter) {
    body += LoadLocal(this_variable_);
    body += LoadField(field);
    body += Return();
  } else {
    body += LoadLocal(this_variable_);
    body += LoadLocal(setter_value);

    if (false && FLAG_use_field_guards) {
      // FIXME(kustermann): Should we implement this? What are field guards?
      //         t1 <- StoreLocal(:expr_temp @-3, t1)
      //         GuardFieldClass:4(field <nullable Null>, t1)
      //         t1 <- LoadLocal(:expr_temp @-3)
      //         GuardFieldLength:6(field <nullable Null>, t1)
      //         t1 <- LoadLocal(:expr_temp @-3)
    }

    body += StoreInstanceField(field);
    body += NullConstant();
    body += Return();
  }

  return new(Z) FlowGraph(*parsed_function_, graph_entry, next_block_id_ - 1);
}


void FlowGraphBuilder::AdjustTemporaries(int base) {
  for (std::vector<LocalVariable*>::iterator it = temporaries_.begin();
       it != temporaries_.end();
       ++it) {
    (*it)->AdjustIndex(base);
  }
}


ArgumentArray FlowGraphBuilder::GetArguments(int count) {
  ArgumentArray arguments =
      new(Z) ZoneGrowableArray<PushArgumentInstr*>(Z, count);
  for (int i = count - 1; i >= 0; --i) {
    ASSERT(stack_->definition()->IsPushArgument());
    ASSERT(!stack_->definition()->HasSSATemp());
    arguments->Add(stack_->definition()->AsPushArgument());
    Drop();
  }
  pending_argument_count_ -= count;
  ASSERT(pending_argument_count_ >= 0);
  return arguments;
}


void FlowGraphBuilder::VisitNullLiteral(NullLiteral* node) {
  fragment_ = Fragment(Constant(Instance::ZoneHandle(Z, Instance::null())));
}


void FlowGraphBuilder::VisitBoolLiteral(BoolLiteral* node) {
  fragment_ = Fragment(Constant(Bool::Get(node->value())));
}


void FlowGraphBuilder::VisitIntLiteral(IntLiteral* node) {
  fragment_ = Fragment(Constant(
      Integer::ZoneHandle(Z, Integer::New(node->value(), Heap::kOld))));
}


void FlowGraphBuilder::VisitBigintLiteral(BigintLiteral* node) {
  const dart::String& value = H.DartString(node->value());
  fragment_ = Fragment(Constant(
      Integer::ZoneHandle(Z, Integer::New(value, Heap::kOld))));
}


void FlowGraphBuilder::VisitDoubleLiteral(DoubleLiteral* node) {
  const dart::String& value = H.DartString(node->value());
  fragment_ = Fragment(Constant(
      Double::ZoneHandle(Z, Double::New(value, Heap::kOld))));
}


void FlowGraphBuilder::VisitStringLiteral(StringLiteral* node) {
  fragment_ = Fragment(Constant(H.DartString(node->value(), Heap::kOld)));
}


class DartTypeTranslator : public DartTypeVisitor {
 public:
    explicit DartTypeTranslator(FlowGraphBuilder* owner)
      : owner_(owner),
        result_(AbstractType::ZoneHandle(owner->zone_)) {
  }

  AbstractType& result() { return result_; }

  void VisitDefaultDartType(DartType* node) { UNREACHABLE(); }

  void VisitInterfaceType(InterfaceType* node);

 private:
  FlowGraphBuilder* owner_;
  AbstractType& result_;
};


void DartTypeTranslator::VisitInterfaceType(InterfaceType* node) {
  if (node->type_arguments().length() != 0) UNIMPLEMENTED();

  const dart::Class& klass =
      dart::Class::Handle(owner_->LookupClassByName(node->klass()->name()));
  result_ ^= klass.DeclarationType();
}


void FlowGraphBuilder::VisitTypeLiteral(TypeLiteral* node) {
  DartTypeTranslator translator(this);
  node->type()->AcceptDartTypeVisitor(&translator);
  fragment_ = Fragment(Constant(translator.result()));
}


void FlowGraphBuilder::VisitVariableGet(VariableGet* node) {
  fragment_ = LoadLocal(LookupVariable(node->variable()));
}


void FlowGraphBuilder::VisitVariableSet(VariableSet* node) {
  LocalVariable* local = LookupVariable(node->variable());
  Fragment instructions = TranslateExpression(node->expression());
  fragment_ = instructions + StoreLocal(local);
}


void FlowGraphBuilder::VisitStaticGet(StaticGet* node) {
  Member* target = node->target();
  if (target->IsField()) {
    const dart::String& field_name = H.DartString(target->name()->string());
    const dart::Field& field =
        dart::Field::ZoneHandle(Z, LookupFieldByName(field_name));
    const dart::Class& owner = dart::Class::Handle(field.Owner());
    const dart::String& getter_name =
        dart::String::Handle(dart::Field::GetterName(field_name));
    const Function& getter =
        Function::ZoneHandle(Z, owner.LookupStaticFunction(getter_name));
    if (getter.IsNull() || field.is_const()) {
      Fragment instructions = Constant(field);
      fragment_ = instructions + LoadStaticField();
    } else {
      // TODO(kmillikin): figure out how to trigger this case and add tests.
      fragment_ = StaticCall(getter, 0);
    }
  } else {
    ASSERT(target->IsProcedure());

    // Invoke the getter function
    Procedure* procedure = Procedure::Cast(target);
    const Function& target = Function::ZoneHandle(Z,
        LookupStaticMethodByDilProcedure(procedure));

    fragment_ = StaticCall(target, 0);
  }
}


void FlowGraphBuilder::VisitStaticSet(StaticSet* node) {
  Member* target = node->target();
  if (target->IsField()) {
    const dart::Field& field =
        dart::Field::ZoneHandle(Z, LookupFieldByName(target->name()->string()));

    Fragment instructions = TranslateExpression(node->expression());
    LocalVariable* variable = MakeTemporary();
    instructions += LoadLocal(variable);
    fragment_ = instructions + StoreStaticField(field);
  } else {
    ASSERT(target->IsProcedure());

    // Evaluate the expression on the right hand side.
    Fragment instructions = TranslateExpression(node->expression());
    LocalVariable* variable = MakeTemporary();

    // Prepare argument.
    instructions += LoadLocal(variable);
    instructions += PushArgument();

    // Invoke the setter function.
    Procedure* procedure = Procedure::Cast(target);
    const Function& target = Function::ZoneHandle(Z,
        LookupStaticMethodByDilProcedure(procedure));
    instructions += StaticCall(target, 1);

    // Drop the unused result & leave the stored value on the stack.
    fragment_ = instructions + Drop();
  }
}


void FlowGraphBuilder::VisitPropertyGet(PropertyGet* node) {
  Fragment instructions = TranslateExpression(node->receiver());
  instructions += PushArgument();
  const dart::String& getter_name = H.DartGetterName(node->name()->string());
  fragment_ = instructions + InstanceCall(getter_name, Token::kGET, 1);
}


void FlowGraphBuilder::VisitPropertySet(PropertySet* node) {
  Fragment instructions(NullConstant());
  LocalVariable* variable = MakeTemporary();
  instructions += TranslateExpression(node->receiver());
  instructions += PushArgument();
  instructions += TranslateExpression(node->value());
  instructions += StoreLocal(variable);
  instructions += PushArgument();

  const dart::String& setter_name = H.DartSetterName(node->name()->string());
  instructions += InstanceCall(setter_name, Token::kSET, 2);
  fragment_ = instructions + Drop();
}


void FlowGraphBuilder::VisitStaticInvocation(StaticInvocation* node) {
  Array& argument_names = Array::ZoneHandle(Z);
  Fragment instructions =
      TranslateArguments(node->arguments(), &argument_names);

  const Function& target = Function::ZoneHandle(Z,
      LookupStaticMethodByDilProcedure(node->procedure()));
  int argument_count = node->arguments()->count();
  fragment_ = instructions +
      StaticCall(target, argument_count, argument_names);
}


void FlowGraphBuilder::VisitMethodInvocation(MethodInvocation* node) {
  Fragment instructions = TranslateExpression(node->receiver());
  instructions += PushArgument();

  Array& argument_names = Array::ZoneHandle(Z);
  instructions += TranslateArguments(node->arguments(), &argument_names);

  const dart::String& name = H.DartSymbol(node->name()->string());  // NOLINT
  int argument_count = node->arguments()->count() + 1;
  fragment_ = instructions +
      InstanceCall(name, Token::kILLEGAL, argument_count, argument_names);
}


void FlowGraphBuilder::VisitConstructorInvocation(ConstructorInvocation* node) {
  const dart::Class& klass = dart::Class::ZoneHandle(
      Z, LookupClassByDilClass(Class::Cast(node->target()->parent())));
  Fragment instructions = AllocateObject(klass);
  LocalVariable* variable = MakeTemporary();

  instructions += LoadLocal(variable);
  instructions += PushArgument();

  Array& argument_names = Array::ZoneHandle(Z);
  instructions += TranslateArguments(node->arguments(), &argument_names);

  const Function& target = Function::ZoneHandle(Z,
      LookupConstructorByDilConstructor(klass, node->target()));
  int argument_count = node->arguments()->count() + 1;
  instructions += StaticCall(target, argument_count, argument_names);
  fragment_ = instructions + Drop();
}


void FlowGraphBuilder::VisitIsExpression(IsExpression* node) {
  Fragment instructions = TranslateExpression(node->operand());
  instructions += PushArgument();
  instructions += NullConstant();
  instructions += PushArgument();  // Type arguments.

  DartTypeTranslator translator(this);
  node->type()->AcceptDartTypeVisitor(&translator);
  instructions += Constant(translator.result());
  instructions += PushArgument();  // Type.

  instructions += Constant(Bool::False());
  instructions += PushArgument();  // Negate?.

  fragment_ = instructions +
      InstanceCall(dart::Library::PrivateCoreLibName(Symbols::_instanceOf()),
                   Token::kIS,
                   4);
}


void FlowGraphBuilder::VisitAsExpression(AsExpression* node) {
  Fragment instructions = TranslateExpression(node->operand());
  instructions += PushArgument();
  instructions += NullConstant();
  instructions += PushArgument();  // Type arguments.

  DartTypeTranslator translator(this);
  node->type()->AcceptDartTypeVisitor(&translator);
  instructions += Constant(translator.result());
  instructions += PushArgument();  // Type.

  fragment_ = instructions +
      InstanceCall(dart::Library::PrivateCoreLibName(Symbols::_as()),
                   Token::kAS,
                   3);
}


void FlowGraphBuilder::VisitConditionalExpression(ConditionalExpression* node) {
  Fragment instructions = TranslateExpression(node->condition());
  TargetEntryInstr* then_entry;
  TargetEntryInstr* otherwise_entry;
  instructions += Branch(&then_entry, &otherwise_entry);

  Value* top = stack_;
  Fragment then_fragment(then_entry);
  then_fragment += TranslateExpression(node->then());
  then_fragment += StoreLocal(parsed_function_->expression_temp_var());
  then_fragment += Drop();

  ASSERT(stack_ == top);
  Fragment otherwise_fragment(otherwise_entry);
  otherwise_fragment += TranslateExpression(node->otherwise());
  otherwise_fragment += StoreLocal(parsed_function_->expression_temp_var());
  otherwise_fragment += Drop();

  JoinEntryInstr* join =
      new(Z) JoinEntryInstr(AllocateBlockId(),
                            CatchClauseNode::kInvalidTryIndex);
  then_fragment += Goto(join);
  otherwise_fragment += Goto(join);

  fragment_ = Fragment(instructions.entry, join) +
      LoadLocal(parsed_function_->expression_temp_var());
}


void FlowGraphBuilder::VisitLogicalExpression(LogicalExpression* node) {
  if (node->op() == LogicalExpression::kAnd ||
      node->op() == LogicalExpression::kOr) {
    Fragment instructions = TranslateExpression(node->left());
    TargetEntryInstr* right_entry;
    TargetEntryInstr* constant_entry;

    if (node->op() == LogicalExpression::kAnd) {
      instructions += Branch(&right_entry, &constant_entry);
    } else {
      instructions += Branch(&constant_entry, &right_entry);
    }

    Value* top = stack_;
    Fragment right_fragment(right_entry);
    right_fragment += TranslateExpression(node->right());
    right_fragment += Boolify();
    right_fragment += StoreLocal(parsed_function_->expression_temp_var());
    right_fragment += Drop();

    ASSERT(top == stack_);
    Fragment constant_fragment(constant_entry);
    constant_fragment +=
        Constant(Bool::Get(node->op() == LogicalExpression::kOr));
    constant_fragment += StoreLocal(parsed_function_->expression_temp_var());
    constant_fragment += Drop();

    JoinEntryInstr* join =
        new(Z) JoinEntryInstr(AllocateBlockId(),
                              CatchClauseNode::kInvalidTryIndex);
    right_fragment += Goto(join);
    constant_fragment += Goto(join);

    fragment_ = Fragment(instructions.entry, join)
        + LoadLocal(parsed_function_->expression_temp_var());
  } else {
    UNIMPLEMENTED();
  }
}


void FlowGraphBuilder::VisitNot(Not* node) {
  Fragment instructions = TranslateExpression(node->expression());
  fragment_ = instructions + BooleanNegate();
}


void FlowGraphBuilder::VisitThisExpression(ThisExpression* node) {
  fragment_ = LoadLocal(this_variable_);
}


Fragment FlowGraphBuilder::TranslateArguments(Arguments* node,
                                              Array* argument_names) {
  if (node->types().length() != 0) {
    UNIMPLEMENTED();
  }
  Fragment instructions;

  List<Expression>& positional = node->positional();
  List<NamedExpression>& named = node->named();
  if (named.length() == 0) {
    *argument_names ^= Array::null();
  } else {
    *argument_names ^= Array::New(named.length());
  }

  for (int i = 0; i < positional.length(); ++i) {
    instructions += TranslateExpression(positional[i]);
    instructions += PushArgument();
  }
  for (int i = 0; i < named.length(); ++i) {
    NamedExpression* named_expression = named[i];
    instructions += TranslateExpression(named_expression->expression());
    instructions += PushArgument();
    argument_names->SetAt(i, H.DartSymbol(named_expression->name()));
  }
  return instructions;
}


void FlowGraphBuilder::VisitEmptyStatement(EmptyStatement* node) {
  fragment_ = Fragment();
}


void FlowGraphBuilder::VisitBlock(Block* node) {
  scope_ = new LocalScope(scope_, 0, loop_depth_);
  Fragment instructions;
  List<Statement>& statements = node->statements();
  for (int i = 0; i < statements.length(); ++i) {
    instructions += TranslateStatement(statements[i]);
  }
  fragment_ = instructions;
  scope_ = scope_->parent();
}


void FlowGraphBuilder::VisitReturnStatement(ReturnStatement* node) {
  Fragment instructions = node->expression() == NULL
      ? NullConstant()
      : TranslateExpression(node->expression());
  instructions += Return();
  fragment_ = instructions.closed();
}


void FlowGraphBuilder::VisitExpressionStatement(ExpressionStatement* node) {
  Fragment instructions = TranslateExpression(node->expression());
  fragment_ = instructions + Drop();
}


void FlowGraphBuilder::VisitVariableDeclaration(VariableDeclaration* node) {
  const dart::String& symbol = H.DartSymbol(node->name());
  LocalVariable* local =
    new(Z) LocalVariable(TokenPosition::kNoSource, symbol,
                         Type::ZoneHandle(Z, Type::DynamicType()));
  AddVariable(node, local);

  Fragment instructions = node->initializer() == NULL
      ? NullConstant()
      : TranslateExpression(node->initializer());
  instructions += StoreLocal(local);
  fragment_ = instructions + Drop();
}


void FlowGraphBuilder::VisitIfStatement(IfStatement* node) {
  Fragment instructions = TranslateExpression(node->condition());
  TargetEntryInstr* then_entry;
  TargetEntryInstr* otherwise_entry;
  instructions += Branch(&then_entry, &otherwise_entry);

  Fragment then_fragment(then_entry);
  then_fragment += TranslateStatement(node->then());

  Fragment otherwise_fragment(otherwise_entry);
  otherwise_fragment += TranslateStatement(node->otherwise());

  if (then_fragment.is_open()) {
    if (otherwise_fragment.is_open()) {
      JoinEntryInstr* join =
          new(Z) JoinEntryInstr(AllocateBlockId(),
                                CatchClauseNode::kInvalidTryIndex);
      then_fragment += Goto(join);
      otherwise_fragment += Goto(join);
      fragment_ = Fragment(instructions.entry, join);
    } else {
      fragment_ = Fragment(instructions.entry, then_fragment.current);
    }
  } else if (otherwise_fragment.is_open()) {
    fragment_ = Fragment(instructions.entry, otherwise_fragment.current);
  } else {
    fragment_ = instructions.closed();
  }
}


void FlowGraphBuilder::VisitWhileStatement(WhileStatement* node) {
  ++loop_depth_;
  Fragment condition = TranslateExpression(node->condition());
  TargetEntryInstr* body_entry;
  TargetEntryInstr* loop_exit;
  condition += Branch(&body_entry, &loop_exit);

  Fragment body(body_entry);
  body += TranslateStatement(node->body());

  Instruction* entry;
  if (body.is_open()) {
    JoinEntryInstr* join =
        new(Z) JoinEntryInstr(AllocateBlockId(),
                              CatchClauseNode::kInvalidTryIndex);
    body += Goto(join);

    Fragment loop(join);
    loop += CheckStackOverflow();
    loop += condition;
    entry = new(Z) GotoInstr(join);
  } else {
    entry = condition.entry;
  }


  fragment_ = Fragment(entry, loop_exit);
  --loop_depth_;
}


void FlowGraphBuilder::VisitDoStatement(DoStatement* node) {
  ++loop_depth_;
  Fragment body = TranslateStatement(node->body());

  if (body.is_closed()) {
    fragment_ = body;
    --loop_depth_;
    return;
  }

  JoinEntryInstr* join =
      new(Z) JoinEntryInstr(AllocateBlockId(),
                            CatchClauseNode::kInvalidTryIndex);
  Fragment loop(join);
  loop += CheckStackOverflow();
  loop += body;
  loop += TranslateExpression(node->condition());
  TargetEntryInstr* loop_repeat;
  TargetEntryInstr* loop_exit;
  loop += Branch(&loop_repeat, &loop_exit);

  Fragment repeat(loop_repeat);
  repeat += Goto(join);

  fragment_ = Fragment(new(Z) GotoInstr(join), loop_exit);
  --loop_depth_;
}


void FlowGraphBuilder::VisitForStatement(ForStatement* node) {
  scope_ = new LocalScope(scope_, 0, loop_depth_);
  Fragment declarations;
  List<VariableDeclaration>& variables = node->variables();
  for (int i = 0; i < variables.length(); ++i) {
    declarations += TranslateStatement(variables[i]);
  }

  ++loop_depth_;
  Fragment condition = TranslateExpression(node->condition());
  TargetEntryInstr* body_entry;
  TargetEntryInstr* loop_exit;
  condition += Branch(&body_entry, &loop_exit);

  Fragment body(body_entry);
  body += TranslateStatement(node->body());

  if (body.is_open()) {
    List<Expression>& updates = node->updates();
    for (int i = 0; i < updates.length(); ++i) {
      body += TranslateExpression(updates[i]);
      body += Drop();
    }
    JoinEntryInstr* join =
        new(Z) JoinEntryInstr(AllocateBlockId(),
                              CatchClauseNode::kInvalidTryIndex);
    declarations += Goto(join);
    body += Goto(join);

    Fragment loop(join);
    loop += CheckStackOverflow();
    loop += condition;
  } else {
    declarations += condition;
  }

  fragment_ = Fragment(declarations.entry, loop_exit);
  --loop_depth_;
  scope_ = scope_->parent();
}


void FlowGraphBuilder::VisitForInStatement(ForInStatement* node) {
  Fragment instructions = TranslateExpression(node->iterable());
  instructions += PushArgument();

  const dart::String& iterator_getter = dart::String::ZoneHandle(Z,
      dart::Field::GetterSymbol(Symbols::Iterator()));
  instructions += InstanceCall(iterator_getter, Token::kGET, 1);
  LocalVariable* iterator = MakeTemporary();

  ++loop_depth_;
  Fragment condition = LoadLocal(iterator);
  condition += PushArgument();
  condition += InstanceCall(Symbols::MoveNext(), Token::kILLEGAL, 1);
  TargetEntryInstr* body_entry;
  TargetEntryInstr* loop_exit;
  condition += Branch(&body_entry, &loop_exit);

  scope_ = new LocalScope(scope_, 0, loop_depth_);
  const dart::String& symbol = H.DartSymbol(node->variable()->name());
  LocalVariable* variable =
      new(Z) LocalVariable(TokenPosition::kNoSource, symbol,
                           Type::ZoneHandle(Z, Type::DynamicType()));
  AddVariable(node->variable(), variable);
  Fragment body(body_entry);
  body += LoadLocal(iterator);
  body += PushArgument();
  const dart::String& current_getter = dart::String::ZoneHandle(Z,
      dart::Field::GetterSymbol(Symbols::Current()));
  body += InstanceCall(current_getter, Token::kGET, 1);
  body += StoreLocal(variable);
  body += TranslateStatement(node->body());

  if (body.is_open()) {
    JoinEntryInstr* join =
        new(Z) JoinEntryInstr(AllocateBlockId(),
                              CatchClauseNode::kInvalidTryIndex);
    instructions += Goto(join);
    body += Goto(join);

    Fragment loop(join);
    loop += CheckStackOverflow();
    loop += condition;
  } else {
    instructions += condition;
  }

  fragment_ = Fragment(instructions.entry, loop_exit) + Drop();
  --loop_depth_;
  scope_ = scope_->parent();
}


}  // namespace dil
}  // namespace dart
