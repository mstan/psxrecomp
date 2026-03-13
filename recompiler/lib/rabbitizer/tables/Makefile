C_TABLE_GEN         := tools/c_table_gen.sh
RS_TABLE_GEN        := tools/rs_table_gen.sh
PYI_TABLE_GEN       := tools/pyi_table_gen.sh

IINC                := -I tables

TEMPLATE_FOLDER     := templates

$(shell mkdir -p ../include/generated/ ../cplusplus/include/generated/)


TABLE_C_TEMPLATES           := $(wildcard $(TEMPLATE_FOLDER)/c/*.table.template)
TABLE_C_GENERATED           := $(TABLE_C_TEMPLATES:$(TEMPLATE_FOLDER)/c/%.table.template=../include/generated/%.h)

TABLE_CPLUSPLUS_TEMPLATES   := $(wildcard $(TEMPLATE_FOLDER)/cplusplus/*.table.template)
TABLE_CPLUSPLUS_GENERATED   := $(TABLE_CPLUSPLUS_TEMPLATES:$(TEMPLATE_FOLDER)/cplusplus/%.table.template=../cplusplus/include/generated/%.hpp)

TABLE_RS_TEMPLATES          := $(wildcard $(TEMPLATE_FOLDER)/rust/*.tablers.template)
TABLE_RS_GENERATED          := $(TABLE_RS_TEMPLATES:$(TEMPLATE_FOLDER)/rust/%.tablers.template=../rust/src/%.rs)

TABLE_PYI_TEMPLATES         := $(wildcard $(TEMPLATE_FOLDER)/python/*.tablepyi.template)
TABLE_PYI_GENERATED         := $(TABLE_PYI_TEMPLATES:$(TEMPLATE_FOLDER)/python/%.tablepyi.template=../rabbitizer/%.pyi)


TABLE_DEP_FILES     += $(TABLE_C_GENERATED:%.h=%.d)
TABLE_DEP_FILES     += $(TABLE_CPLUSPLUS_GENERATED:%.hpp=%.d)
TABLE_DEP_FILES     += $(TABLE_RS_TEMPLATES:%.template=%.d)
TABLE_DEP_FILES     += $(TABLE_PYI_TEMPLATES:%.template=%.d)


all: tables

tables: $(TABLE_C_GENERATED) $(TABLE_CPLUSPLUS_GENERATED) $(TABLE_RS_GENERATED) $(TABLE_PYI_GENERATED)
	cargo fmt

clean:
	$(RM) -rf $(TABLE_C_GENERATED)
	$(RM) -rf $(TABLE_CPLUSPLUS_GENERATED)
	$(RM) -rf $(TABLE_RS_GENERATED)
	$(RM) -rf $(TABLE_PYI_GENERATED)

distclean: clean
	$(RM) -rf $(TABLE_DEP_FILES)


.PHONY: all tables clean distclean
.DEFAULT_GOAL := all
.SECONDARY:


../include/generated/%.h: $(TEMPLATE_FOLDER)/c/%.table.template
	cpp -P $(IINC) -M -MM -MMD -MP -MT $@ -MF $(@:.h=.d) $<
	$(C_TABLE_GEN) $< $@ $(@F) || rm -rf $@

../cplusplus/include/generated/%.hpp: $(TEMPLATE_FOLDER)/cplusplus/%.table.template
	cpp -P $(IINC) -M -MM -MMD -MP -MT $@ -MF $(@:.hpp=.d) $<
	$(C_TABLE_GEN) $< $@ $(@F) || rm -rf $@

../rust/src/%.rs: $(TEMPLATE_FOLDER)/rust/%.tablers.template
	cpp -P $(IINC) -M -MM -MMD -MP -MT $@ -MF $(<:.template=.d) $<
	$(RS_TABLE_GEN) $< $@ || rm -rf $@

../rabbitizer/%.pyi: $(TEMPLATE_FOLDER)/python/%.tablepyi.template
	cpp -P $(IINC) -M -MM -MMD -MP -MT $@ -MF $(<:.template=.d) $<
	$(PYI_TABLE_GEN) $< $@ || rm -rf $@

-include $(TABLE_DEP_FILES)

# Print target for debugging
print-% : ; $(info $* is a $(flavor $*) variable set to [$($*)]) @true
