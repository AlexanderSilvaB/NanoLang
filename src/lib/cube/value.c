#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "object.h"
#include "memory.h"
#include "value.h"
#include "vm.h"

static Obj *allocateObject(size_t size, ObjType type, bool isList)
{
  Obj *object = (Obj *)reallocate(NULL, 0, size);
  object->type = type;

  if (!isList)
  {
    object->next = vm.objects;
    vm.objects = object;
  }
  else
  {
    object->next = vm.listObjects;
    vm.listObjects = object;
  }

#ifdef DEBUG_TRACE_GC
  printf("%p allocate %ld for %d\n", (void *)object, size, type);
#endif

  return object;
}

#define ALLOCATE_OBJ_LIST(type, objectType) \
  (type *)allocateObject(sizeof(type), objectType, true)

void initValueArray(ValueArray *array)
{
  array->values = NULL;
  array->capacity = 0;
  array->count = 0;
}

void writeValueArray(ValueArray *array, Value value)
{
  if (array->capacity < array->count + 1)
  {
    int oldCapacity = array->capacity;
    array->capacity = GROW_CAPACITY(oldCapacity);
    array->values = GROW_ARRAY(array->values, Value,
                               oldCapacity, array->capacity);
  }

  array->values[array->count] = value;
  array->count++;
}

void freeValueArray(ValueArray *array)
{
  FREE_ARRAY(Value, array->values, array->capacity);
  initValueArray(array);
}

static uint32_t hash(char *str)
{
  uint32_t hash = 5381;
  int c;

  while ((c = *str++))
    hash = ((hash << 5) + hash) + c;

  return hash;
}

ObjDict *initDictValues(uint32_t capacity)
{
  ObjDict *dict = ALLOCATE_OBJ_LIST(ObjDict, OBJ_DICT);
  dict->capacity = capacity;
  dict->count = 0;
  dict->items = calloc(capacity, sizeof(*dict->items));

  return dict;
}

void insertDict(ObjDict *dict, char *key, Value value)
{
  if (dict->count * 100 / dict->capacity >= 60)
    resizeDict(dict, true);

  uint32_t hashValue = hash(key);
  int index = hashValue % dict->capacity;
  char *key_m = ALLOCATE(char, strlen(key) + 1);

  if (!key_m)
  {
    printf("ERROR!");
    return;
  }

  strcpy(key_m, key);

  dictItem *item = ALLOCATE(dictItem, sizeof(dictItem));

  if (!item)
  {
    printf("ERROR!");
    return;
  }

  item->key = key_m;
  item->item = value;
  item->deleted = false;
  item->hash = hashValue;

  while (dict->items[index] && !dict->items[index]->deleted &&
         strcmp(dict->items[index]->key, key) != 0)
  {
    index++;
    if (index == dict->capacity)
      index = 0;
  }

  if (dict->items[index])
  {
    free(dict->items[index]->key);
    free(dict->items[index]);
    dict->count--;
  }

  dict->items[index] = item;
  dict->count++;
}

void resizeDict(ObjDict *dict, bool grow)
{
  int newSize;

  if (grow)
    newSize = dict->capacity << 1; // Grow by a factor of 2
  else
    newSize = dict->capacity >> 1; // Shrink by a factor of 2

  dictItem **items = calloc(newSize, sizeof(*dict->items));

  for (int j = 0; j < dict->capacity; ++j)
  {
    if (!dict->items[j])
      continue;
    if (dict->items[j]->deleted)
      continue;

    int index = dict->items[j]->hash % newSize;

    while (items[index])
      index = (index + 1) % newSize;

    items[index] = dict->items[j];
  }

  // Free deleted values
  for (int j = 0; j < dict->capacity; ++j)
  {
    if (!dict->items[j])
      continue;
    if (dict->items[j]->deleted)
      freeDictValue(dict->items[j]);
  }

  free(dict->items);

  dict->capacity = newSize;
  dict->items = items;
}

Value searchDict(ObjDict *dict, char *key)
{
  int index = hash(key) % dict->capacity;

  if (!dict->items[index])
    return NONE_VAL;

  while (index < dict->capacity && dict->items[index] &&
         !dict->items[index]->deleted &&
         strcmp(dict->items[index]->key, key) != 0)
  {
    index++;
    if (index == dict->capacity)
    {
      index = 0;
    }
  }

  if (dict->items[index] && !dict->items[index]->deleted)
  {
    return dict->items[index]->item;
  }

  return NONE_VAL;
}

// Calling function needs to free memory
char *valueToString(Value value)
{
  if (IS_BOOL(value))
  {
    char *str = AS_BOOL(value) ? "true" : "false";
    char *boolString = malloc(sizeof(char) * (strlen(str) + 1));
    snprintf(boolString, strlen(str) + 1, "%s", str);
    return boolString;
  }
  else if (IS_NONE(value))
  {
    char *nilString = malloc(sizeof(char) * 5);
    snprintf(nilString, 5, "%s", "none");
    return nilString;
  }
  else if (IS_NUMBER(value))
  {
    double number = AS_NUMBER(value);
    int numberStringLength = snprintf(NULL, 0, "%.15g", number) + 1;
    char *numberString = malloc(sizeof(char) * numberStringLength);
    snprintf(numberString, numberStringLength, "%.15g", number);
    return numberString;
  }
  else if (IS_OBJ(value))
  {
    return objectToString(value);
  }

  char *unknown = malloc(sizeof(char) * 8);
  snprintf(unknown, 8, "%s", "unknown");
  return unknown;
}

char *valueType(Value value)
{
  if (IS_BOOL(value))
  {
    char *str = malloc(sizeof(char) * 6);
		snprintf(str, 5, "bool");
		return str;
  }
  else if (IS_NONE(value))
  {
    char *str = malloc(sizeof(char) * 6);
		snprintf(str, 5, "none");
		return str;
  }
  else if (IS_NUMBER(value))
  {
    char *str = malloc(sizeof(char) * 5);
		snprintf(str, 4, "num");
		return str;
  }
  else if (IS_OBJ(value))
  {
    return objectType(value);
  }

  char *unknown = malloc(sizeof(char) * 8);
  snprintf(unknown, 8, "unknown");
  return unknown;
}

void printValue(Value value)
{
  char *output = valueToString(value);
  printf("%s", output);
  free(output);
}

bool valuesEqual(Value a, Value b) 
{
#ifdef NAN_TAGGING
  if(IS_OBJ(a) && IS_OBJ(b))
  {
    return objectComparison(a, b);
  }

	return a == b;
#else
	if (a.type != b.type)
		return false;

	switch (a.type) {
		case VAL_BOOL:
			return AS_BOOL(a) == AS_BOOL(b);
		case VAL_NONE:
			return true;
		case VAL_NUMBER:
			return AS_NUMBER(a) == AS_NUMBER(b);
		case VAL_OBJ:
			return objectComparison(a, b);
	}
#endif
}

Value toBool(Value value)
{
  if (IS_NONE(value))
    return BOOL_VAL(false);
  else if (IS_BOOL(value))
    return BOOL_VAL(AS_BOOL(value));
  else if (IS_NUMBER(value))
  {
    return BOOL_VAL(AS_NUMBER(value) > 0);
  }
  else if (IS_STRING(value))
  {
    char *str = AS_CSTRING(value);
    if (strlen(str) == 0 || strcmp(str, "0") == 0 || strcmp(str, "false") == 0)
      return BOOL_VAL(false);
    else
      return BOOL_VAL(false);
  }
  return BOOL_VAL(true);
}

Value toNumber(Value value)
{
  if (IS_NONE(value))
    return NUMBER_VAL(0);
  else if (IS_BOOL(value))
    return NUMBER_VAL(AS_BOOL(value) ? 1 : 0);
  else if (IS_NUMBER(value))
  {
    return NUMBER_VAL(AS_NUMBER(value));
  }
  else if (IS_STRING(value))
  {
    char *str = AS_CSTRING(value);
    double value = strtod(str, NULL);
    return NUMBER_VAL(value);
  }
  return NUMBER_VAL(1);
}

Value toString(Value value)
{
  char *str = valueToString(value);
  Value v = STRING_VAL(str);
  free(str);
  return v;
}