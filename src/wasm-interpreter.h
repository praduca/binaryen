//
// Simple WebAssembly interpreter, designed to be embeddable in JavaScript, so it
// can function as a polyfill.
//

#include <limits.h>

#include "wasm.h"

namespace wasm {

using namespace cashew;

//
// An instance of a WebAssembly module, which can execute it via AST interpretation
//

IString WASM("wasm");

class ModuleInstance {
public:
  typedef std::vector<Literal> LiteralList;

  //
  // You need to implement one of these to create a concrete interpreter. The
  // ExternalInterface provides embedding-specific functionality like calling
  // an imported function or accessing memory.
  //
  struct ExternalInterface {
    virtual void init(Module& wasm) {}
    virtual Literal callImport(Import* import, LiteralList& arguments) = 0;
    virtual Literal load(Load* load, size_t addr) = 0;
    virtual void store(Store* store, size_t addr, Literal value) = 0;
    virtual void trap() = 0;
  };

  ModuleInstance(Module& wasm, ExternalInterface* externalInterface) : wasm(wasm), externalInterface(externalInterface) {
    for (auto function : wasm.functions) {
      functions[function->name] = function;
    }
    memorySize = wasm.memory.initial;
    externalInterface->init(wasm);
  }

#ifdef WASM_INTERPRETER_DEBUG
  int indent = 0;
#endif

  //
  // Calls a function. This can be used both internally (calls from
  // the interpreter to another method), or when you want to call into
  // the module.
  //
  Literal callFunction(IString name, LiteralList& arguments) {

    class FunctionScope {
    public:
      std::map<IString, Literal> locals;
      Function* function;

      FunctionScope(Function* function, LiteralList& arguments) : function(function) {
        assert(function->params.size() == arguments.size());
        for (size_t i = 0; i < arguments.size(); i++) {
          assert(function->params[i].type == arguments[i].type);
          locals[function->params[i].name] = arguments[i];
        }
        for (auto& local : function->locals) {
          locals[local.name].type = local.type;
        }
      }
    };

    // Stuff that flows around during executing expressions: a literal, or a change in control flow
    class Flow {
    public:
      Flow() {}
      Flow(Literal value) : value(value) {}
      Flow(IString breakTo) : breakTo(breakTo) {}

      Literal value;
      IString breakTo; // if non-null, a break is going on

      bool breaking() { return breakTo.is(); }

      void clearIf(IString target) {
        if (breakTo == target) {
          breakTo.clear();
        }
      }

      std::ostream& print(std::ostream& o) {
        o << "(flow " << (breakTo.is() ? breakTo.str : "-") << " : " << value << ')';
        return o;
      }
    };

#ifdef WASM_INTERPRETER_DEBUG
    struct IndentHandler {
      int& indent;
      const char *name;
      IndentHandler(int& indent, const char *name, Expression *expression) : indent(indent), name(name) {
        doIndent(std::cout, indent);
        std::cout << "visit " << name << " :\n";
        indent++;
        doIndent(std::cout, indent);
        expression->print(std::cout, indent) << '\n';
        indent++;
      }
      ~IndentHandler() {
        indent--;
        indent--;
        doIndent(std::cout, indent);
        std::cout << "exit " << name << '\n';
      }
    };
    #define NOTE_ENTER(x) IndentHandler indentHandler(instance.indent, x, curr);
    #define NOTE_EVAL() { doIndent(std::cout, instance.indent); std::cout << "eval " << indentHandler.name << '\n'; }
    #define NOTE_EVAL1(p0) { doIndent(std::cout, instance.indent); std::cout << "eval " << indentHandler.name << '('  << p0 << ")\n"; }
    #define NOTE_EVAL2(p0, p1) { doIndent(std::cout, instance.indent); std::cout << "eval " << indentHandler.name << '('  << p0 << ", " << p1 << ")\n"; }
#else
    #define NOTE_ENTER(x)
    #define NOTE_EVAL()
    #define NOTE_EVAL1(p0)
    #define NOTE_EVAL2(p0, p1)
#endif

    // Execute a statement
    class ExpressionRunner : public WasmVisitor<Flow> {
      ModuleInstance& instance;
      FunctionScope& scope;

    public:
      ExpressionRunner(ModuleInstance& instance, FunctionScope& scope) : instance(instance), scope(scope) {}

      Flow visitBlock(Block *curr) override {
        NOTE_ENTER("Block");
        Flow flow;
        for (auto expression : curr->list) {
          flow = visit(expression);
          if (flow.breaking()) {
            flow.clearIf(curr->name);
            return flow;
          }
        }
        return flow;
      }
      Flow visitIf(If *curr) override {
        NOTE_ENTER("If");
        Flow flow = visit(curr->condition);
        if (flow.breaking()) return flow;
        NOTE_EVAL1(flow.value);
        if (flow.value.geti32()) return visit(curr->ifTrue);
        if (curr->ifFalse) return visit(curr->ifFalse);
        return Flow();
      }
      Flow visitLoop(Loop *curr) override {
        NOTE_ENTER("Loop");
        while (1) {
          Flow flow = visit(curr->body);
          if (flow.breaking()) {
            if (flow.breakTo == curr->in) continue; // lol
            flow.clearIf(curr->out);
            return flow;
          }
        }
      }
      Flow visitLabel(Label *curr) override {
        NOTE_ENTER("Label");
        Flow flow = visit(curr->body);
        flow.clearIf(curr->name);
        return flow;
      }
      Flow visitBreak(Break *curr) override {
        NOTE_ENTER("Break");
        if (curr->value) {
          Flow flow = visit(curr->value);
          if (!flow.breaking()) {
            flow.breakTo = curr->name;
          }
          return flow;
        }
        return Flow(curr->name);
      }
      Flow visitSwitch(Switch *curr) override {
        NOTE_ENTER("Switch");
        Flow flow = visit(curr->value);
        if (flow.breaking()) {
          flow.clearIf(curr->name);
          return flow;
        }
        NOTE_EVAL1(flow.value);
        int32_t index = flow.value.geti32();
        Name target = curr->default_;
        if (index >= 0 && index < curr->targets.size()) {
          target = curr->targets[index];
        }
        auto iter = curr->caseMap.find(target);
        if (iter == curr->caseMap.end()) {
          // not in the cases, so this is a break outside
          return Flow(target);
        }
        size_t caseIndex = iter->second;
        assert(caseIndex < curr->cases.size());
        while (caseIndex < curr->cases.size()) {
          Switch::Case& c = curr->cases[caseIndex];
          Flow flow = visit(c.body);
          if (flow.breaking()) {
            flow.clearIf(curr->name);
            return flow;
          }
          caseIndex++;
        }
        return Flow();
      }

      Flow generateArguments(const ExpressionList& operands, LiteralList& arguments) {
        arguments.reserve(operands.size());
        for (auto expression : operands) {
          Flow flow = visit(expression);
          if (flow.breaking()) return flow;
          arguments.push_back(flow.value);
        }
        return Flow();
      }

      Flow visitCall(Call *curr) override {
        NOTE_ENTER("Call");
        LiteralList arguments;
        Flow flow = generateArguments(curr->operands, arguments);
        if (flow.breaking()) return flow;
        return instance.callFunction(curr->target, arguments);
      }
      Flow visitCallImport(CallImport *curr) override {
        NOTE_ENTER("CallImport");
        LiteralList arguments;
        Flow flow = generateArguments(curr->operands, arguments);
        if (flow.breaking()) return flow;
        return instance.externalInterface->callImport(instance.wasm.importsMap[curr->target], arguments);
      }
      Flow visitCallIndirect(CallIndirect *curr) override {
        NOTE_ENTER("CallIndirect");
        Flow target = visit(curr->target);
        if (target.breaking()) return target;
        size_t index = target.value.geti32();
        if (index >= instance.wasm.table.names.size()) trap();
        Name name = instance.wasm.table.names[index];
        Function *func = instance.wasm.functionsMap[name];
        if (func->type.is() && func->type != curr->type->name) trap();
        LiteralList arguments;
        Flow flow = generateArguments(curr->operands, arguments);
        if (flow.breaking()) return flow;
        return instance.callFunction(name, arguments);
      }

      Name getLocalName(Name name) {
        if (scope.locals.find(name) != scope.locals.end()) {
          return name;
        }
        // this is a numeric index
        size_t id = std::stoi(name.str);
        size_t numParams = scope.function->params.size();
        if (id < numParams) {
          return scope.function->params[id].name;
        }
        id -= numParams;
        assert(id < scope.function->locals.size());
        return scope.function->locals[id].name;
      }

      Flow visitGetLocal(GetLocal *curr) override {
        NOTE_ENTER("GetLocal");
        IString name = getLocalName(curr->name);
        NOTE_EVAL1(scope.locals[name]);
        return scope.locals[name];
      }
      Flow visitSetLocal(SetLocal *curr) override {
        NOTE_ENTER("SetLocal");
        IString name = getLocalName(curr->name);
        Flow flow = visit(curr->value);
        if (flow.breaking()) return flow;
        NOTE_EVAL1(flow.value);
        scope.locals[name] = flow.value;
        return flow;
      }
      Flow visitLoad(Load *curr) override {
        NOTE_ENTER("Load");
        Flow flow = visit(curr->ptr);
        if (flow.breaking()) return flow;
        return instance.externalInterface->load(curr, instance.getFinalAddress(curr, flow.value));
      }
      Flow visitStore(Store *curr) override {
        NOTE_ENTER("Store");
        Flow ptr = visit(curr->ptr);
        if (ptr.breaking()) return ptr;
        Flow value = visit(curr->value);
        if (value.breaking()) return value;
        instance.externalInterface->store(curr, instance.getFinalAddress(curr, ptr.value), value.value);
        return value;
      }
      Flow visitConst(Const *curr) override {
        NOTE_ENTER("Const");
        NOTE_EVAL1(curr->value);
        return Flow(curr->value); // heh
      }
      Flow visitUnary(Unary *curr) override {
        NOTE_ENTER("Unary");
        Flow flow = visit(curr->value);
        if (flow.breaking()) return flow;
        Literal value = flow.value;
        NOTE_EVAL1(value);
        if (value.type == i32) {
          int32_t v = value.geti32();
          switch (curr->op) {
            case Clz: {
              if (v == 0) return Literal(32);
              return Literal((int32_t)__builtin_clz(v));
            }
            case Ctz: {
              if (v == 0) return Literal(32);
              return Literal((int32_t)__builtin_ctz(v));
            }
            case Popcnt: return Literal((int32_t)__builtin_popcount(v));
            default: abort();
          }
        }
        if (value.type == i64) {
          int64_t v = value.geti64();
          switch (curr->op) {
            case Clz: {
              if (v == 0) return Literal(32);
              return Literal((int64_t)__builtin_clz(v));
            }
            case Ctz: {
              if (v == 0) return Literal(32);
              return Literal((int64_t)__builtin_ctz(v));
            }
            case Popcnt: return Literal((int64_t)__builtin_popcount(v));
            default: abort();
          }
        }
        if (value.type == f32) {
          float v = value.getf32();
          switch (curr->op) {
            case Neg:     return Literal(-v);
            case Abs:     return Literal(std::abs(v));
            case Ceil:    return Literal(std::ceil(v));
            case Floor:   return Literal(std::floor(v));
            case Trunc:   return Literal(std::trunc(v));
            case Nearest: return Literal(std::nearbyint(v));
            case Sqrt:    return Literal(std::sqrt(v));
            default: abort();
          }
        }
        if (value.type == f64) {
          double v = value.getf64();
          switch (curr->op) {
            case Neg:     return Literal(-v);
            case Abs:     return Literal(std::abs(v));
            case Ceil:    return Literal(std::ceil(v));
            case Floor:   return Literal(std::floor(v));
            case Trunc:   return Literal(std::trunc(v));
            case Nearest: return Literal(std::nearbyint(v));
            case Sqrt:    return Literal(std::sqrt(v));
            default: abort();
          }
        }
        abort();
      }
      Flow visitBinary(Binary *curr) override {
        NOTE_ENTER("Binary");
        Flow flow = visit(curr->left);
        if (flow.breaking()) return flow;
        Literal left = flow.value;
        flow = visit(curr->right);
        if (flow.breaking()) return flow;
        Literal right = flow.value;
        NOTE_EVAL2(left, right);
        if (left.type == i32) {
          int32_t l = left.geti32(), r = right.geti32();
          switch (curr->op) {
            case Add:      return Literal(l + r);
            case Sub:      return Literal(l - r);
            case Mul:      return Literal(l * r);
            case DivS: {
              if (r == 0) trap();
              if (l == INT32_MIN && r == -1) trap(); // signed division overflow
              return Literal(l / r);
            }
            case DivU: {
              if (r == 0) trap();
              return Literal(int32_t(uint32_t(l) / uint32_t(r)));
            }
            case RemS: {
              if (r == 0) trap();
              if (l == INT32_MIN && r == -1) return Literal(int32_t(0));
              return Literal(l % r);
            }
            case RemU: {
              if (r == 0) trap();
              return Literal(int32_t(uint32_t(l) % uint32_t(r)));
            }
            case And:      return Literal(l & r);
            case Or:       return Literal(l | r);
            case Xor:      return Literal(l ^ r);
            case Shl: {
              if (uint32_t(r) >= 32) return Literal(int32_t(0));
              return Literal(l << r);
            }
            case ShrU: {
              if (uint32_t(r) >= 32) return Literal(int32_t(0));
              return Literal(int32_t(uint32_t(l) >> uint32_t(r)));
            }
            case ShrS: {
              if (uint32_t(r) >= 32) return Literal(int32_t(l < 0 ? -1 : 0));
              return Literal(l >> r);
            }
            default: abort();
          }
        } else if (left.type == i64) {
          int64_t l = left.geti64(), r = right.geti64();
          switch (curr->op) {
            case Add:      return Literal(l + r);
            case Sub:      return Literal(l - r);
            case Mul:      return Literal(l * r);
            case DivS: {
              if (r == 0) trap();
              if (l == LLONG_MIN && r == -1) trap(); // signed division overflow
              return Literal(l / r);
            }
            case DivU: {
              if (r == 0) trap();
              return Literal(int64_t(uint64_t(l) / uint64_t(r)));
            }
            case RemS: {
              if (r == 0) trap();
              if (l == LLONG_MIN && r == -1) return Literal(int64_t(0));
              return Literal(l % r);
            }
            case RemU: {
              if (r == 0) trap();
              return Literal(int64_t(uint64_t(l) % uint64_t(r)));
            }
            case And:      return Literal(l & r);
            case Or:       return Literal(l | r);
            case Xor:      return Literal(l ^ r);
            case Shl: {
              if (uint64_t(r) >= 64) return Literal(int64_t(0));
              return Literal(l << r);
            }
            case ShrU: {
              if (uint64_t(r) >= 64) return Literal(int64_t(0));
              return Literal(int64_t(uint64_t(l) >> uint64_t(r)));
            }
            case ShrS: {
              if (uint64_t(r) >= 64) return Literal(int64_t(l < 0 ? -1 : 0));
              return Literal(l >> r);
            }
            default: abort();
          }
        } else if (left.type == f32) {
          float l = left.getf32(), r = right.getf32();
          switch (curr->op) {
            case Add:      return Literal(l + r);
            case Sub:      return Literal(l - r);
            case Mul:      return Literal(l * r);
            case Div:      return Literal(l / r);
            case CopySign: return Literal(std::copysign(l, r));
            case Min: {
              if (l == r && l == 0) return Literal(1/l < 0 ? l : r);
              return Literal(std::min(l, r));
            }
            case Max: {
              if (l == r && l == 0) return Literal(1/l < 0 ? r : l);
              return Literal(std::max(l, r));
            }
            default: abort();
          }
        } else if (left.type == f64) {
          double l = left.getf64(), r = right.getf64();
          switch (curr->op) {
            case Add:      return Literal(l + r);
            case Sub:      return Literal(l - r);
            case Mul:      return Literal(l * r);
            case Div:      return Literal(l / r);
            case CopySign: return Literal(std::copysign(l, r));
            case Min: {
              if (l == r && l == 0) return Literal(1/l < 0 ? l : r);
              return Literal(std::min(l, r));
            }
            case Max: {
              if (l == r && l == 0) return Literal(1/l < 0 ? r : l);
              return Literal(std::max(l, r));
            }
            default: abort();
          }
        }
        abort();
      }
      Flow visitCompare(Compare *curr) override {
        NOTE_ENTER("Compare");
        Flow flow = visit(curr->left);
        if (flow.breaking()) return flow;
        Literal left = flow.value;
        flow = visit(curr->right);
        if (flow.breaking()) return flow;
        Literal right = flow.value;
        NOTE_EVAL2(left, right);
        if (left.type == i32) {
          int32_t l = left.geti32(), r = right.geti32();
          switch (curr->op) {
            case Eq:  return Literal(l == r);
            case Ne:  return Literal(l != r);
            case LtS: return Literal(l < r);
            case LtU: return Literal(uint32_t(l) < uint32_t(r));
            case LeS: return Literal(l <= r);
            case LeU: return Literal(uint32_t(l) <= uint32_t(r));
            case GtS: return Literal(l > r);
            case GtU: return Literal(uint32_t(l) > uint32_t(r));
            case GeS: return Literal(l >= r);
            case GeU: return Literal(uint32_t(l) >= uint32_t(r));
            default: abort();
          }
        } else if (left.type == i64) {
          int64_t l = left.geti64(), r = right.geti64();
          switch (curr->op) {
            case Eq:  return Literal(l == r);
            case Ne:  return Literal(l != r);
            case LtS: return Literal(l < r);
            case LtU: return Literal(uint64_t(l) < uint64_t(r));
            case LeS: return Literal(l <= r);
            case LeU: return Literal(uint64_t(l) <= uint64_t(r));
            case GtS: return Literal(l > r);
            case GtU: return Literal(uint64_t(l) > uint64_t(r));
            case GeS: return Literal(l >= r);
            case GeU: return Literal(uint64_t(l) >= uint64_t(r));
            default: abort();
          }
        } else if (left.type == f32) {
          float l = left.getf32(), r = right.getf32();
          switch (curr->op) {
            case Eq:  return Literal(l == r);
            case Ne:  return Literal(l != r);
            case Lt:  return Literal(l <  r);
            case Le:  return Literal(l <= r);
            case Gt:  return Literal(l >  r);
            case Ge:  return Literal(l >= r);
            default: abort();
          }
        } else if (left.type == f64) {
          double l = left.getf64(), r = right.getf64();
          switch (curr->op) {
            case Eq:  return Literal(l == r);
            case Ne:  return Literal(l != r);
            case Lt:  return Literal(l <  r);
            case Le:  return Literal(l <= r);
            case Gt:  return Literal(l >  r);
            case Ge:  return Literal(l >= r);
            default: abort();
          }
        }
        abort();
      }
      Flow visitConvert(Convert *curr) override {
        NOTE_ENTER("Convert");
        Flow flow = visit(curr->value);
        if (flow.breaking()) return flow;
        Literal value = flow.value;
        switch (curr->op) { // :-)
          case ExtendSInt32:     return Literal(int64_t(value.geti32()));
          case ExtendUInt32:     return Literal(uint64_t((uint32_t)value.geti32()));
          case WrapInt64:        return Literal(int32_t(value.geti64()));
          case TruncSFloat32:
          case TruncSFloat64: {
            double val = curr->op == TruncSFloat32 ? value.getf32() : value.getf64();
            if (isnan(val)) trap();
            if (curr->type == i32) {
              if (val > (double)INT_MAX || val < (double)INT_MIN) trap();
              return Literal(int32_t(val));
            } else {
              int64_t converted = val;
              if ((val >= 1 && converted <= 0) || val < (double)LLONG_MIN) trap();
              return Literal(converted);
            }
          }
          case TruncUFloat32:
          case TruncUFloat64: {
            double val = curr->op == TruncUFloat32 ? value.getf32() : value.getf64();
            if (isnan(val)) trap();
            if (curr->type == i32) {
              if (val > (double)UINT_MAX || val <= (double)-1) trap();
              return Literal(uint32_t(val));
            } else {
              uint64_t converted = val;
              if (converted < val - 1 || val <= (double)-1) trap();
              return Literal(converted);
            }
          }
          case ReinterpretFloat: {
            if (value.type == f64 && isnan(value.getf64())) return Literal((int64_t)0x7ff8000000000000); // canonicalized
            return curr->type == i32 ? Literal(value.reinterpreti32()) : Literal(value.reinterpreti64());
          }
          case ConvertUInt32:    return curr->type == f32 ? Literal(float(uint32_t(value.geti32()))) : Literal(double(uint32_t(value.geti32())));
          case ConvertSInt32:    return curr->type == f32 ? Literal(float(int32_t(value.geti32()))) : Literal(double(int32_t(value.geti32())));
          case ConvertUInt64:    return curr->type == f32 ? Literal(float((uint64_t)value.geti64())) : Literal(double((uint64_t)value.geti64()));
          case ConvertSInt64:    return curr->type == f32 ? Literal(float(value.geti64())) : Literal(double(value.geti64()));
          case PromoteFloat32:   return Literal(double(value.getf32()));
          case DemoteFloat64:    return Literal(float(value.getf64()));
          case ReinterpretInt:   return curr->type == f32 ? Literal(value.reinterpretf32()) : Literal(value.reinterpretf64());
          default: abort();
        }
      }
      Flow visitSelect(Select *curr) override {
        NOTE_ENTER("Select");
        Flow condition = visit(curr->condition);
        if (condition.breaking()) return condition;
        NOTE_EVAL1(condition.value);
        Flow ifTrue = visit(curr->ifTrue);
        if (ifTrue.breaking()) return ifTrue;
        Flow ifFalse = visit(curr->ifFalse);
        if (ifFalse.breaking()) return ifFalse;
        return condition.value.geti32() ? ifTrue : ifFalse; // ;-)
      }
      Flow visitHost(Host *curr) override {
        NOTE_ENTER("Host");

        switch (curr->op) {
          case PageSize:   return Literal(64*1024);
          case MemorySize: return Literal(instance.memorySize);
          case GrowMemory: abort();
          case HasFeature: {
            IString id = curr->nameOperand;
            if (id == WASM) return Literal(1);
            return Literal(0);
          }
          default: abort();
        }
      }
      Flow visitNop(Nop *curr) override {
        NOTE_ENTER("Nop");
        return Flow();
      }

      void trap() {
        instance.externalInterface->trap();
      }
    };

    Function *function = functions[name];
    FunctionScope scope(function, arguments);

    Literal ret = ExpressionRunner(*this, scope).visit(function->body).value;
    if (function->result == none) ret = Literal();
    assert(function->result == ret.type);
    return ret;
  }

  // Convenience method, for the case where you have no arguments.
  Literal callFunction(IString name) {
    LiteralList empty;
    return callFunction(name, empty);
  }

  std::map<IString, Function*> functions;
  size_t memorySize;

  template<class LS>
  size_t getFinalAddress(LS *curr, Literal ptr) {
    uint64_t addr = ptr.type == i32 ? ptr.geti32() : ptr.geti64();
    if (memorySize < curr->offset) externalInterface->trap();
    if (addr > memorySize - curr->offset) externalInterface->trap();
    addr += curr->offset;
    assert(memorySize >= curr->bytes);
    if (addr > memorySize - curr->bytes) externalInterface->trap();
    return addr;
  }

private:
  Module& wasm;
  ExternalInterface* externalInterface;
};

} // namespace wasm

