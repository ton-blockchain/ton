struct RustGenerator {

  std::string to_string(int value) {
    std::ostringstream ss;
    ss << value;
    return ss.str();
  }

  std::string tabs2spaces(std::string inp) {
    std::string outp;
    for (auto ch : inp) {
      if (ch == '\t') outp += "    ";
      else outp += ch;
    }
    return outp;
  }

  std::set<std::string> user_defined_types;

  std::string rust_rename(const std::string s) {
    if (s == "anycast_info") return "AnycastInfo";
    if (s == "Anycast") return "AnycastInfo";
    if (s == "addr_extern") return "MsgAddrExt";
    if (s == "addr_std") return "MsgAddrStd";
    if (s == "addr_var") return "MsgAddrVar";
    return s;
  }

  std::string rust_rename2(const std::string s) {
    if (s == "addr_none") return "AddrNone";
    if (s == "addr_extern") {
      if (_current_type->get_name() == "MsgAddress") return "AddrExt";
      else return "AddrExtern";
    }
    if (s == "addr_std") return "AddrStd";
    if (s == "addr_var") return "AddrVar";
    return s;
  }

  std::string to_rust_type(const TypeExpr* expr) {
    const Type* ta = expr->type_applied;

    if (!ta) {
      return "UnknownType:ta=0";
    }

    if (expr->tp == TypeExpr::te_Apply && ta == NatWidth_type) {
      if (expr->args.at(0)->tp == TypeExpr::te_IntConst) {
        return "Number" + to_string(expr->args[0]->value);
      }
    }
    if (ta == Bits_type) {
      return "SliceData";
    }
    auto tname = ta->get_name();
    if (tname == "Maybe") {
      return "Option<" + to_rust_type(expr->args[0]) + ">";
    }
    if (tname == "VarInteger" || tname == "VarUInteger") {
      return tname + "<" + to_string(expr->args[0]->value) + ">";
    }
    if (tname == "int8") return "i8";
    if (tname == "int32") return "i32";
    if (tname == "bits256") return "AccountId";
    if (user_defined_types.count(tname) > 0) {
      return rust_rename(tname);
    }
    // std::cout << "[3181] " << expr->tp << ":";
    // expr->show(std::cout);
    return "UnknownType:" + ta->get_name() + ",tp=" + to_string(expr->tp);
  }

  std::string rust_output_expr(const TypeExpr* expr, const std::vector<std::string>& field_vars) {
    switch (expr->tp) {
      case TypeExpr::te_Apply: {
        return rust_output_expr(expr->args[0], field_vars);
      }
      case TypeExpr::te_Param: {
        return "self." + field_vars.at(expr->value);
      }
    }
    return "???";
  }

  std::string rust_get_field(const Field& field, const std::vector<std::string>& field_vars) {
    auto fname = get_name(field.name);
    const Type* ta = field.type->type_applied;
    std::string s = "self." + fname;
    if (ta == Bits_type) {
      s += " = cell.get_next_slice(" + rust_output_expr(field.type, field_vars) + ".0 as usize)?;";
    } else if (ta->get_name() == "Maybe") {
      s += " = " + to_rust_type(field.type->args[0]) + "::read_maybe_from(cell)?;";
    } else if (ta->get_name() == "bits256") {
      s += " = cell.get_next_slice(256)?;";
    } else {
      s += ".read_from(cell)?;";
    }
    return s + "\n";
  }

  std::string get_name(sym_idx_t name) {
    return sym::symbols.get_name(name);
  }

  int get_constr_tag(Constructor* constr) {
    return int(constr->tag >> (64 - constr->tag_bits));
  }

  const Type* _current_type = nullptr;

  void generate_rust(std::ostream& os, const Type& type) {

    // os << "Addding " << type.get_name() << "\n";
    user_defined_types.insert(type.get_name());

    if (type.get_name() == "Maybe") return;
    if (type.get_name() == "VarInteger") return;
    if (type.get_name() == "VarUInteger") return;

    _current_type = &type;

    for (auto& constr : type.constructors) {

      auto constr_name = get_name(constr->constr_name);
      if (constr_name == "_") continue;

      // os  << "// [" << type.get_name() << "-" << constr_name << "]\n\n";

      if (constr->fields.empty()) continue;

      auto class_name = rust_rename(constr_name);
      std::string s = "#[derive(Clone, Debug, Default, PartialEq, Eq, Hash)]\n";
      s += "pub struct " + class_name + " {\n";

      for (auto& field : constr->fields) {
        auto fname = get_name(field.name);
        if (fname == "_") continue;
        s += "\tpub " + fname + ": " + to_rust_type(field.type) + ",\n";
      }
      s += "}\n\n";

      std::vector<std::string> field_vars;
      for (auto& field : constr->fields) {
        auto fname = get_name(field.name);
        field_vars.push_back(fname);
      }

      s += "impl Deserializable for " + class_name + " {\n"
           + "\tfn read_from(&mut self, cell: &mut SliceData) -> BlockResult<()> {\n";

      for (auto& field : constr->fields) {
        auto fname = get_name(field.name);
        if (fname == "_") continue;
        s += "\t\t" + rust_get_field(field, field_vars);
      }

      s += "\t\tOk(())\n";
      s += "\t}\n";
      s += "}\n\n";

      os << tabs2spaces(s);
    }

    // os << type.useful_depth << type.is_pfx_determ << "\n";

    std::map<const Constructor*, const Type*> owner;

    std::vector<Constructor*> constructors;
    for (auto& constr : type.constructors) {
      if (get_name(constr->constr_name) == "_") {
        Type* type = constr->fields[0].type->type_applied;
        for (auto& constr2 : type->constructors) {
          owner[constr2] = type;
          constructors.push_back(constr2);
        }
      } else {
        owner[constr] = &type;
        constructors.push_back(constr);
      }
    }

    std::sort(constructors.begin(), constructors.end(), [this] (Constructor* a, Constructor* b) {
      return get_constr_tag(a) < get_constr_tag(b);
    });

    if (constructors.size() > 1) {
      std::string s = "#[derive(Clone, Debug, PartialEq, Eq, Hash)]\n";
      s += "pub enum " + type.get_name() + " {\n";

      int tag_bits = 0;

      for (auto& constr : constructors) {
        s += "\t" + rust_rename2(get_name(constr->constr_name));
        if (!constr->fields.empty())
          s += "(" + rust_rename(get_name(constr->constr_name)) + ")";
        s += ",\n";
        // os << "[" << constr->tag_bits << "," << (constr->tag >> (64 - constr->tag_bits)) << "]";
        tag_bits = constr->tag_bits;
      }

      s += "}\n\n";

      s += "impl Deserializable for " + type.get_name() + " {\n";
      s += "\tfn read_from(&mut self, cell: &mut SliceData) -> BlockResult<()> {\n";

      s += "\t\tlet bits = cell.get_next_bits(" + to_string(tag_bits) +")?[0] >> " + to_string(8-tag_bits) + ";\n";
      // TODO: add error checking for invalid bits!
      for (auto& constr : constructors) {
        s += "\t\tif bits == " + to_string(get_constr_tag(constr)) + " {\n";
        if (constr->fields.empty()) {
          s += "\t\t\t*self = " + type.get_name() + "::" + rust_rename2(get_name(constr->constr_name)) + ";\n";
        } else {
          auto tname = get_name(constr->constr_name);
          s += "\t\t\tlet mut data = " + rust_rename(tname) + "::default();\n";
          s += "\t\t\tdata.read_from(cell)?;\n";
          s += "\t\t\t*self = " + type.get_name() + "::" + rust_rename2(tname) + "(data);\n";
        }
        s += "\t\t}\n";
      }
      s += "\t\tOk(())\n";

      s += "\t}\n";
      s += "}\n\n";

      os << tabs2spaces(s);
    }
  }

};

void generate_rust(std::ostream& os, const Type& type) {
  static RustGenerator rg;
  rg.generate_rust(os, type);
}
