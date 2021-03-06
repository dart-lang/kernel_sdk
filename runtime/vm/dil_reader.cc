// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/dil_reader.h"

#include <string.h>

#include "vm/dart_api_impl.h"
#include "vm/longjump.h"
#include "vm/object_store.h"
#include "vm/parser.h"
#include "vm/symbols.h"

namespace dart {
namespace dil {

#define Z (zone_)
#define I (isolate_)
#define T (type_translator_)
#define H (translation_helper_)

// FIXME(kustermann): We should add support for type conversion to annotate
// fields/parameters/variables with proper types.


class SimpleExpressionConverter : public ExpressionVisitor {
 public:
  SimpleExpressionConverter(Thread* thread, Zone* zone)
      : translation_helper_(thread, zone, NULL),
        zone_(zone), is_simple_(false), simple_value_(NULL) {}

  virtual void VisitDefaultExpression(Expression* node) {
    is_simple_ = false;
  }

  virtual void VisitIntLiteral(IntLiteral* node) {
    is_simple_ = true;
    simple_value_ = &Integer::ZoneHandle(
        Z, Integer::New(node->value(), Heap::kOld));
    *simple_value_ = H.Canonicalize(*simple_value_);
  }

  virtual void VisitBigintLiteral(BigintLiteral* node) {
    is_simple_ = true;
    simple_value_ = &Integer::ZoneHandle(
        Z, Integer::New(H.DartString(node->value(), Heap::kOld)));
    *simple_value_ = H.Canonicalize(*simple_value_);
  }

  virtual void VisitDoubleLiteral(DoubleLiteral* node) {
    is_simple_ = true;
    simple_value_ = &Double::ZoneHandle(
        Z, Double::New(H.DartString(node->value()), Heap::kOld));
    *simple_value_ = H.Canonicalize(*simple_value_);
  }

  virtual void VisitBoolLiteral(BoolLiteral* node) {
    is_simple_ = true;
    simple_value_ = &Bool::Handle(Z, Bool::Get(node->value()).raw());
  }

  virtual void VisitNullLiteral(NullLiteral* node) {
    is_simple_ = true;
    simple_value_ = &dart::Instance::ZoneHandle(Z, dart::Instance::null());
  }

  virtual void VisitStringLiteral(StringLiteral* node) {
    is_simple_ = true;
    simple_value_ = &H.DartSymbol(node->value());
  }

  bool IsSimple(Expression* expression) {
    expression->AcceptExpressionVisitor(this);
    return is_simple_;
  }

  const dart::Instance& SimpleValue() { return *simple_value_; }

 private:
  TranslationHelper translation_helper_;
  dart::Zone* zone_;
  bool is_simple_;
  dart::Instance* simple_value_;
};

void BuildingTranslationHelper::SetFinalize(bool finalize) {
  reader_->finalize_ = finalize;
}

RawLibrary* BuildingTranslationHelper::LookupLibraryByDilLibrary(
    Library* library) {
  return reader_->LookupLibrary(library).raw();
}

RawClass* BuildingTranslationHelper::LookupClassByDilClass(Class* klass) {
  return reader_->LookupClass(klass).raw();
}

Program* DilReader::ReadPrecompiledProgram() {
  Program* program = ReadPrecompiledDilFromBuffer(buffer_, buffer_length_);
  if (program == NULL) return NULL;
  intptr_t source_file_count = program->line_starting_table().size();
  scripts_ = Array::New(source_file_count);
  program_ = program;
  return program;
}

Object& DilReader::ReadProgram() {
  ASSERT(!bootstrapping_);
  Program* program = ReadPrecompiledProgram();
  if (program == NULL) {
    const dart::String& error = H.DartString("Failed to read .dill file");
    return Object::Handle(Z, ApiError::New(error));
  }

  LongJumpScope jump;
  if (setjmp(*jump.Set()) == 0) {
    Procedure* main = program->main_method();
    Library* dil_main_library = Library::Cast(main->parent());

    intptr_t length = program->libraries().length();
    for (intptr_t i = 0; i < length; i++) {
      Library* dil_library = program->libraries()[i];
      ReadLibrary(dil_library);
    }

    // We finalize classes after we've constructed all classes since we
    // currently don't construct them in pre-order of the class hierarchy (and
    // finalization of a class needs all of its superclasses to be finalized).
    for (intptr_t i = 0; i < length; i++) {
      Library* dil_library = program->libraries()[i];
      dart::Library& library = LookupLibrary(dil_library);
      if (!library.Loaded()) {
        dart::Class& klass = dart::Class::Handle(Z, library.toplevel_class());
        ClassFinalizer::FinalizeTypesInClass(klass);
        ClassFinalizer::FinalizeClass(klass);
        for (intptr_t i = 0; i < dil_library->classes().length(); i++) {
          klass = LookupClass(dil_library->classes()[i]).raw();
          ClassFinalizer::FinalizeTypesInClass(klass);
          ClassFinalizer::FinalizeClass(klass);
        }
        library.SetLoaded();
      }
    }

    dart::Library& library = LookupLibrary(dil_main_library);

    // Sanity check that we can find the main entrypoint.
    Object& main_obj = Object::Handle(Z,
        library.LookupObjectAllowPrivate(H.DartSymbol("main")));
    ASSERT(!main_obj.IsNull());

    return library;
  } else {
    // Everything else is a compile-time error. We don't use the [error] since
    // it sometimes causes the higher-level error handling to try to read the
    // script and token position (which we don't have) to produce a nice error
    // message.
    Error& error = Error::Handle(Z);
    error = thread_->sticky_error();
    thread_->clear_sticky_error();

    // Instead we simply make a non-informative error message.
    const dart::String& error_message =
        H.DartString("Failed to read .dill file => CompileTimeError.");
    return Object::Handle(Z, LanguageError::New(error_message));
  }
}

void DilReader::ReadLibrary(Library* dil_library) {
  dart::Library& library = LookupLibrary(dil_library);
  if (library.Loaded()) return;

  // The bootstrapper will take care of creating the native wrapper classes, but
  // we will add the synthetic constructors to them here.
  if (library.name() ==
      Symbols::Symbol(Symbols::kDartNativeWrappersLibNameId).raw()) {
    ASSERT(library.LoadInProgress());
  } else {
    library.SetLoadInProgress();
  }
  // Setup toplevel class (which contains library fields/procedures).
  Script& script = ScriptAt(dil_library->source_uri_index());
  dart::Class& toplevel_class = dart::Class::Handle(dart::Class::New(
      library, Symbols::TopLevel(), script, TokenPosition::kNoSource));
  toplevel_class.set_is_cycle_free();
  library.set_toplevel_class(toplevel_class);
  if (bootstrapping_) {
    GrowableObjectArray::Handle(I->object_store()->pending_classes())
        .Add(toplevel_class, Heap::kOld);
  }

  ActiveClassScope active_class_scope(&active_class_, NULL, &toplevel_class);
  // Load toplevel fields.
  for (intptr_t i = 0; i < dil_library->fields().length(); i++) {
    Field* dil_field = dil_library->fields()[i];

    ActiveMemberScope active_member_scope(&active_class_, dil_field);
    const dart::String& name = H.DartFieldName(dil_field->name());
    dart::Field& field = dart::Field::Handle(Z, dart::Field::NewTopLevel(
        name,
        dil_field->IsFinal(),
        dil_field->IsConst(),
        ClassForScriptAt(toplevel_class, dil_field->source_uri_index()),
        dil_field->position()));
    field.set_dil_field(reinterpret_cast<intptr_t>(dil_field));
    const AbstractType& type = T.TranslateType(dil_field->type());
    field.SetFieldType(type);
    field.set_has_initializer(dil_field->initializer() != NULL);
    GenerateFieldAccessors(toplevel_class, field, dil_field);
    toplevel_class.AddField(field);
    library.AddObject(field, name);
  }

  // Load toplevel procedures.
  for (intptr_t i = 0; i < dil_library->procedures().length(); i++) {
    Procedure* dil_procedure = dil_library->procedures()[i];
    ReadProcedure(library, toplevel_class, dil_procedure);
  }

  // Load all classes.
  for (intptr_t i = 0; i < dil_library->classes().length(); i++) {
    Class* dil_klass = dil_library->classes()[i];
    ReadClass(library, dil_klass);
  }
}

void DilReader::ReadPreliminaryClass(dart::Class* klass, Class* dil_klass) {
  ActiveClassScope active_class_scope(&active_class_, dil_klass, klass);

  // First setup the type parameters, so if any of the following code uses it
  // (in a recursive way) we're fine.
  TypeArguments& type_parameters =
      TypeArguments::Handle(Z, TypeArguments::null());
  intptr_t num_type_parameters = dil_klass->type_parameters().length();
  if (num_type_parameters > 0) {
    dart::TypeParameter& parameter = dart::TypeParameter::Handle(Z);
    Type& null_bound = Type::Handle(Z, Type::null());

    // Step a) Create array of [TypeParameter] objects (without bound).
    type_parameters = TypeArguments::New(num_type_parameters);
    for (intptr_t i = 0; i < num_type_parameters; i++) {
      parameter = dart::TypeParameter::New(
          *klass,
          i,
          H.DartSymbol(dil_klass->type_parameters()[i]->name()),
          null_bound,
          TokenPosition::kNoSource);
      type_parameters.SetTypeAt(i, parameter);
    }
    klass->set_type_parameters(type_parameters);

    // Step b) Fill in the bounds of all [TypeParameter]s.
    for (intptr_t i = 0; i < num_type_parameters; i++) {
      TypeParameter* dil_parameter = dil_klass->type_parameters()[i];
      // There is no dynamic bound, only Object.
      // TODO(kustermann): Should we fix this in the kernel IR generator?
      if (dil_parameter->bound()->IsDynamicType()) {
        parameter ^= type_parameters.TypeAt(i);
        parameter.set_bound(Type::Handle(Z, I->object_store()->object_type()));
      } else {
        AbstractType& bound = T.TranslateTypeWithoutFinalization(
            dil_parameter->bound());
        if (bound.IsMalformedOrMalbounded()) {
          bound = I->object_store()->object_type();
        }

        parameter ^= type_parameters.TypeAt(i);
        parameter.set_bound(bound);
      }
    }
  }

  if (dil_klass->IsNormalClass()) {
    NormalClass* dil_normal_class = NormalClass::Cast(dil_klass);

    // Set super type.  Some classes (e.g., Object) do not have one.
    if (dil_normal_class->super_class() != NULL) {
      AbstractType& super_type = T.TranslateTypeWithoutFinalization(
          dil_normal_class->super_class());
      if (super_type.IsMalformed()) H.ReportError("Malformed super type");
      klass->set_super_type(super_type);
    }
  } else {
    MixinClass* dil_mixin = MixinClass::Cast(dil_klass);

    // Set super type.
    AbstractType& super_type = T.TranslateTypeWithoutFinalization(
        dil_mixin->first());
    if (super_type.IsMalformed()) H.ReportError("Malformed super type.");
    klass->set_super_type(super_type);

    // Tell the rest of the system there is nothing to resolve.
    super_type.SetIsResolved();

    // Set mixin type.
    AbstractType& mixin_type = T.TranslateTypeWithoutFinalization(
        dil_mixin->second());
    if (mixin_type.IsMalformed()) H.ReportError("Malformed mixin type.");
    klass->set_mixin(Type::Cast(mixin_type));
  }

  // Build implemented interface types
  intptr_t interface_count = dil_klass->implemented_classes().length();
  const dart::Array& interfaces =
      dart::Array::Handle(Z, dart::Array::New(interface_count));
  dart::Class& interface_class = dart::Class::Handle(Z);
  for (intptr_t i = 0; i < interface_count; i++) {
    InterfaceType* dil_interface_type = dil_klass->implemented_classes()[i];
    const AbstractType& type = T.TranslateTypeWithoutFinalization(
        dil_interface_type);
    if (type.IsMalformed()) H.ReportError("Malformed interface type.");
    interfaces.SetAt(i, type);

    // NOTE: Normally the DartVM keeps a list of pending classes and iterates
    // through them later on using `ClassFinalizer::ProcessPendingClasses()`.
    // This involes calling `ClassFinalizer::ResolveSuperTypeAndInterfaces()`
    // which does a lot of error validation (e.g. cycle checks) which we don't
    // need here.  But we do need to do one thing which this resolving phase
    // normally does for us: set the `is_implemented` boolean.

    // TODO(kustermann): Maybe we can do this differently once we have
    // "bootstrapping from dill"-support.
    interface_class = type.type_class();
    interface_class.set_is_implemented();
  }
  klass->set_interfaces(interfaces);
  if (dil_klass->is_abstract()) klass->set_is_abstract();
  klass->set_is_cycle_free();

  // When bootstrapping we should not finalize types yet because they will be
  // finalized when the object store's pending_classes list is drained by
  // ClassFinalizer::ProcessPendingClasses.  Even when not bootstrapping we are
  // careful not to eagerly finalize types that may introduce a circularity
  // (such as type arguments, interface types, field types, etc.).
  if (finalize_) ClassFinalizer::FinalizeTypesInClass(*klass);
}

void DilReader::ReadClass(const dart::Library& library, Class* dil_klass) {
  // This will trigger a call to [ReadPreliminaryClass] if not already done.
  dart::Class& klass = LookupClass(dil_klass);

  ActiveClassScope active_class_scope(&active_class_, dil_klass, &klass);

  for (intptr_t i = 0; i < dil_klass->fields().length(); i++) {
    Field* dil_field = dil_klass->fields()[i];
    ActiveMemberScope active_member_scope(&active_class_, dil_field);

    const dart::String& name = H.DartFieldName(dil_field->name());
    // TODO(vegorov) check if this might have some ordering issues
    // e.g. addressing types that are not loaded yet.
    const AbstractType& type =
        T.TranslateTypeWithoutFinalization(dil_field->type());
    dart::Field& field = dart::Field::Handle(Z,
        dart::Field::New(name,
                         dil_field->IsStatic(),
                         // In the VM all const fields are implicitly final
                         // whereas in Kernel they are not final because they
                         // are not explicitly declared that way.
                         dil_field->IsFinal() || dil_field->IsConst(),
                         dil_field->IsConst(),
                         false,  // is_reflectable
                         klass,
                         type,
                         dil_field->position()));
    field.set_dil_field(reinterpret_cast<intptr_t>(dil_field));
    field.set_has_initializer(dil_field->initializer() != NULL);
    GenerateFieldAccessors(klass, field, dil_field);
    klass.AddField(field);
  }

  for (intptr_t i = 0; i < dil_klass->constructors().length(); i++) {
    Constructor* dil_constructor = dil_klass->constructors()[i];
    ActiveMemberScope active_member_scope(&active_class_, dil_constructor);
    ActiveFunctionScope active_function_scope(
        &active_class_, dil_constructor->function());

    const dart::String& name = H.DartConstructorName(dil_constructor);
    Function& function = dart::Function::ZoneHandle(Z, dart::Function::New(
          name,
          RawFunction::kConstructor,
          false,  // is_static
          dil_constructor->IsConst(),
          false,  // is_abstract
          dil_constructor->IsExternal(),
          false,  // is_native
          klass,
          TokenPosition::kNoSource));
    klass.AddFunction(function);
    function.set_dil_function(reinterpret_cast<intptr_t>(dil_constructor));
    function.set_result_type(T.ReceiverType(klass));
    SetupFunctionParameters(H, T, klass, function, dil_constructor->function(),
                            true,    // is_method
                            false);  // is_closure
  }

  for (intptr_t i = 0; i < dil_klass->procedures().length(); i++) {
    Procedure* dil_procedure = dil_klass->procedures()[i];
    ActiveMemberScope active_member_scope(&active_class_, dil_procedure);
    ReadProcedure(library, klass, dil_procedure, dil_klass);
  }

  if (bootstrapping_ && !klass.is_marked_for_parsing()) {
    klass.set_is_marked_for_parsing();
    GrowableObjectArray::Handle(Z, I->object_store()->pending_classes())
        .Add(klass, Heap::kOld);
  }
}

void DilReader::ReadProcedure(const dart::Library& library,
                              const dart::Class& owner,
                              Procedure* dil_procedure,
                              Class* dil_klass) {
  ActiveClassScope active_class_scope(&active_class_, dil_klass, &owner);
  ActiveMemberScope active_member_scope(&active_class_, dil_procedure);
  ActiveFunctionScope active_function_scope(
      &active_class_, dil_procedure->function());

  const dart::String& name = H.DartProcedureName(dil_procedure);
  bool is_method = dil_klass != NULL && !dil_procedure->IsStatic();
  bool is_abstract = dil_procedure->IsAbstract();
  bool is_external = dil_procedure->IsExternal();
  dart::String* native_name = NULL;
  if (is_external) {
    // Maybe it has a native implementation, which is not external as far as
    // the VM is concerned because it does have an implementation.  Check for
    // an ExternalName annotation and extract the string from it.
    for (int i = 0; i < dil_procedure->annotations().length(); ++i) {
      Expression* annotation = dil_procedure->annotations()[i];
      if (!annotation->IsConstructorInvocation()) continue;
      ConstructorInvocation* invocation =
          ConstructorInvocation::Cast(annotation);
      Class* annotation_class = Class::Cast(invocation->target()->parent());
      String* class_name = annotation_class->name();
      // Just compare by name, do not generate the annotation class.
      int length = sizeof("ExternalName") - 1;
      if (class_name->size() != length) continue;
      if (memcmp(class_name->buffer(), "ExternalName", length) != 0) continue;
      String* library_name = annotation_class->parent()->name();
      length = sizeof("dart._internal") - 1;
      if (library_name->size() != length) continue;
      if (memcmp(library_name->buffer(), "dart._internal", length) != 0) {
        continue;
      }

      is_external = false;
      ASSERT(invocation->arguments()->positional().length() == 1 &&
             invocation->arguments()->named().length() == 0);
      StringLiteral* literal =
          StringLiteral::Cast(invocation->arguments()->positional()[0]);
      native_name = &H.DartSymbol(literal->value());
      break;
    }
  }

  dart::Function& function = dart::Function::ZoneHandle(Z, Function::New(
        name,
        GetFunctionType(dil_procedure),
        !is_method,  // is_static
        false,  // is_const
        is_abstract,
        is_external,
        native_name != NULL,  // is_native
        ClassForScriptAt(owner, dil_procedure->source_uri_index()),
        TokenPosition::kNoSource));
  owner.AddFunction(function);
  function.set_dil_function(reinterpret_cast<intptr_t>(dil_procedure));
  function.set_is_debuggable(false);
  if (native_name != NULL) {
    function.set_native_name(*native_name);
  }

  SetupFunctionParameters(H, T, owner, function, dil_procedure->function(),
                          is_method,
                          false);  // is_closure

  if (dil_klass == NULL) {
    library.AddObject(function, name);
    ASSERT(!Object::Handle(library.LookupObjectAllowPrivate(
        H.DartProcedureName(dil_procedure))).IsNull());
  }
}

const Object& DilReader::ClassForScriptAt(const dart::Class& klass,
        intptr_t source_uri_index) {
  Script& correct_script = ScriptAt(source_uri_index);
  if (klass.script() != correct_script.raw()) {
    // TODO(jensj): We could probably cache this so we don't create
    // new PatchClasses all the time
    return PatchClass::ZoneHandle(Z, PatchClass::New(klass, correct_script));
  }
  return klass;
}

Script& DilReader::ScriptAt(intptr_t source_uri_index) {
  Script& script = Script::ZoneHandle(Z);
  script ^= scripts_.At(source_uri_index);
  if (script.IsNull()) {
    String* uri = program_->source_uri_table().strings()[source_uri_index];
    script = Script::New(H.DartString(uri),
          dart::String::ZoneHandle(Z),
          RawScript::kDilTag);
    scripts_.SetAt(source_uri_index, script);
    intptr_t* line_starts = program_->line_starting_table()
          .valuesFor(source_uri_index);
    intptr_t line_count = line_starts[0];
    Array& array_object = Array::Handle(Z, Array::New(line_count));
    Smi& value = Smi::Handle(Z);
    for (intptr_t i = 0; i < line_count; ++i) {
      value = Smi::New(line_starts[i + 1]);
      array_object.SetAt(i, value);
    }
    script.set_line_starts(array_object);
  }
  return script;
}

void DilReader::GenerateFieldAccessors(const dart::Class& klass,
                                       const dart::Field& field,
                                       Field* dil_field) {
  if (dil_field->IsStatic() && dil_field->initializer() != NULL) {
    // Static fields with initializers either have the static value set to the
    // initializer value if it is simple enough or else set to an uninitialized
    // sentinel.
    SimpleExpressionConverter converter(H.thread(), Z);
    if (converter.IsSimple(dil_field->initializer())) {
      // We do not need a getter.
      field.SetStaticValue(converter.SimpleValue(), true);
      return;
    }
    // We do need a getter that evaluates the initializer if necessary.
    field.SetStaticValue(Object::sentinel(), true);
  }

  const dart::String& getter_name = H.DartGetterName(dil_field->name());
  Function& getter = Function::ZoneHandle(Z, Function::New(
      getter_name,
      dil_field->IsStatic()
          ? RawFunction::kImplicitStaticFinalGetter
          : RawFunction::kImplicitGetter,
      dil_field->IsStatic(),
      // The functions created by the parser have is_const for static fields
      // that are const (not just final) and they have is_const for non-static
      // fields that are final.
      dil_field->IsStatic() ? dil_field->IsConst() : dil_field->IsFinal(),
      false,  // is_abstract
      false,  // is_external
      false,  // is_native
      ClassForScriptAt(klass, dil_field->source_uri_index()),
      dil_field->position()));
  klass.AddFunction(getter);
  if (klass.IsTopLevel()) {
    dart::Library& library = dart::Library::Handle(Z, klass.library());
    library.AddObject(getter, getter_name);
  }
  getter.set_dil_function(reinterpret_cast<intptr_t>(dil_field));
  getter.set_result_type(AbstractType::Handle(Z, field.type()));
  getter.set_is_debuggable(false);
  SetupFieldAccessorFunction(klass, getter);

  if (!dil_field->IsStatic() && !dil_field->IsFinal()) {
    // Only static fields can be const.
    ASSERT(!dil_field->IsConst());
    const dart::String& setter_name = H.DartSetterName(dil_field->name());
    Function& setter = Function::ZoneHandle(Z, Function::New(
          setter_name,
          RawFunction::kImplicitSetter,
          false,  // is_static
          false,  // is_const
          false,  // is_abstract
          false,  // is_external
          false,  // is_native
          ClassForScriptAt(klass, dil_field->source_uri_index()),
          dil_field->position()));
    klass.AddFunction(setter);
    setter.set_dil_function(reinterpret_cast<intptr_t>(dil_field));
    setter.set_result_type(Object::void_type());
    setter.set_is_debuggable(false);
    SetupFieldAccessorFunction(klass, setter);
  }
}

void DilReader::SetupFunctionParameters(TranslationHelper translation_helper_,
                                        DartTypeTranslator type_translator_,
                                        const dart::Class& klass,
                                        const dart::Function& function,
                                        FunctionNode* node,
                                        bool is_method,
                                        bool is_closure) {
  ASSERT(!(is_method && is_closure));
  bool is_factory = function.IsFactory();
  intptr_t extra_parameters = (is_method || is_closure || is_factory) ? 1 : 0;

  function.set_num_fixed_parameters(
      extra_parameters + node->required_parameter_count());
  if (node->named_parameters().length() > 0) {
    function.SetNumOptionalParameters(
        node->named_parameters().length(), false);
  } else {
    function.SetNumOptionalParameters(
        node->positional_parameters().length() -
        node->required_parameter_count(),
        true);
  }
  intptr_t num_parameters =
      extra_parameters +
      node->positional_parameters().length() +
      node->named_parameters().length();
  function.set_parameter_types(
      Array::Handle(Array::New(num_parameters, Heap::kOld)));
  function.set_parameter_names(
      Array::Handle(Array::New(num_parameters, Heap::kOld)));
  intptr_t pos = 0;
  if (is_method) {
    ASSERT(!klass.IsNull());
    function.SetParameterTypeAt(pos, H.GetCanonicalType(klass));
    function.SetParameterNameAt(pos, Symbols::This());
    pos++;
  } else if (is_closure) {
    function.SetParameterTypeAt(pos, AbstractType::dynamic_type());
    function.SetParameterNameAt(pos, Symbols::ClosureParameter());
    pos++;
  } else if (is_factory) {
    function.SetParameterTypeAt(pos, AbstractType::dynamic_type());
    function.SetParameterNameAt(pos, Symbols::TypeArgumentsParameter());
    pos++;
  }
  for (intptr_t i = 0; i < node->positional_parameters().length(); i++, pos++) {
    VariableDeclaration* dil_variable = node->positional_parameters()[i];
    const AbstractType& type =
        T.TranslateTypeWithoutFinalization(dil_variable->type());
    function.SetParameterTypeAt(pos,
        type.IsMalformed() ? Type::dynamic_type() : type);
    function.SetParameterNameAt(pos, H.DartSymbol(dil_variable->name()));
  }
  for (intptr_t i = 0; i < node->named_parameters().length(); i++, pos++) {
    VariableDeclaration* named_expression = node->named_parameters()[i];
    const AbstractType& type =
        T.TranslateTypeWithoutFinalization(named_expression->type());
    function.SetParameterTypeAt(pos,
        type.IsMalformed() ? Type::dynamic_type() : type);
    function.SetParameterNameAt(pos, H.DartSymbol(named_expression->name()));
  }

  // The result type for generative constructors has already been set.
  if (!function.IsGenerativeConstructor()) {
    const AbstractType& return_type =
        T.TranslateTypeWithoutFinalization(node->return_type());
    function.set_result_type(
        return_type.IsMalformed() ? Type::dynamic_type() : return_type);
  }
}

void DilReader::SetupFieldAccessorFunction(const dart::Class& klass,
                                           const dart::Function& function) {
  bool is_setter = function.IsImplicitSetterFunction();
  bool is_method = !function.IsStaticFunction();
  intptr_t num_parameters = (is_method ? 1 : 0) + (is_setter ? 1 : 0);

  function.SetNumOptionalParameters(0, false);
  function.set_num_fixed_parameters(num_parameters);
  function.set_parameter_types(
      Array::Handle(Array::New(num_parameters, Heap::kOld)));
  function.set_parameter_names(
      Array::Handle(Array::New(num_parameters, Heap::kOld)));

  intptr_t pos = 0;
  if (is_method) {
    function.SetParameterTypeAt(pos, T.ReceiverType(klass));
    function.SetParameterNameAt(pos, Symbols::This());
    pos++;
  }
  if (is_setter) {
    function.SetParameterTypeAt(pos, AbstractType::dynamic_type());
    function.SetParameterNameAt(pos, Symbols::Value());
    pos++;
  }
}

dart::Library& DilReader::LookupLibrary(Library* library) {
  dart::Library* handle = NULL;
  if (!libraries_.Lookup(library, &handle)) {
    const dart::String& url = H.DartSymbol(library->import_uri());
    handle =
        &dart::Library::Handle(Z, dart::Library::LookupLibrary(thread_, url));
    if (handle->IsNull()) {
      *handle = dart::Library::New(url);
      handle->Register(thread_);
    }
    ASSERT(!handle->IsNull());
    libraries_.Insert(library, handle);
  }
  return *handle;
}

dart::Class& DilReader::LookupClass(Class* klass) {
  dart::Class* handle = NULL;
  if (!classes_.Lookup(klass, &handle)) {
    dart::Library& library = LookupLibrary(klass->parent());
    const dart::String& name = H.DartClassName(klass);
    handle = &dart::Class::Handle(Z, library.LookupClass(name));
    if (handle->IsNull()) {
      // The class needs to have a script because all the functions in the class
      // will inherit it.  The predicate Function::IsOptimizable uses the
      // absence of a script to detect test functions that should not be
      // optimized.  Use a dummy script.
      //
      // TODO(kmillikin): We shouldn't need a dummy script per class.  At the
      // least we could have a singleton.  At best, we'd change IsOptimizable to
      // detect test functions some other way (like simply not setting the
      // optimizable bit on those functions in the first place).

      Script& script = ScriptAt(klass->source_uri_index());
      handle = &dart::Class::Handle(Z,
          dart::Class::New(library, name, script, TokenPosition::kNoSource));
      library.AddClass(*handle);
    } else if (handle->script() == Script::null()) {
      // When bootstrapping we can encounter classes that do not yet have a
      // dummy script.
      Script& script = ScriptAt(klass->source_uri_index());
      handle->set_script(script);
    }
    // Insert the class in the cache before calling ReadPreliminaryClass so
    // we do not risk allocating the class again by calling LookupClass
    // recursively from ReadPreliminaryClass for the same class.
    classes_.Insert(klass, handle);
    if (!handle->is_cycle_free()) {
      ReadPreliminaryClass(handle, klass);
    }
  }
  return *handle;
}

RawFunction::Kind DilReader::GetFunctionType(Procedure* dil_procedure) {
  // TODO(kustermann): Are these correct?
  intptr_t lookuptable[] = {
    RawFunction::kRegularFunction,  // Procedure::kMethod
    RawFunction::kGetterFunction,   // Procedure::kGetter
    RawFunction::kSetterFunction,   // Procedure::kSetter
    RawFunction::kRegularFunction,  // Procedure::kOperator
    RawFunction::kConstructor,      // Procedure::kFactory
  };
  intptr_t kind = static_cast<int>(dil_procedure->kind());
  if (kind == Procedure::kIncompleteProcedure) {
    // TODO(kustermann): Is this correct?
    return RawFunction::kSignatureFunction;
  } else {
    ASSERT(0 <= kind && kind <= Procedure::kFactory);
    return static_cast<RawFunction::Kind>(lookuptable[kind]);
  }
}

}  // namespace dil
}  // namespace dart
