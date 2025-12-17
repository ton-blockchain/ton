import re


def snake_to_camel_case(name: str, suffix: str = "") -> str:
    # Courtesy of http://stackoverflow.com/a/31531797/4759433
    result = re.sub(r"_([a-z])", lambda m: m.group(1).upper(), name)
    result = result[:1].upper() + result[1:].replace("_", "")
    return result + suffix if suffix else result


def get_class_name(name: str, suffix: str = ""):
    if "." in name:
        name = name.replace(".", "_")
    # return snake_to_camel_case(name, suffix)
    return name[0].upper() + name[1:] + suffix
