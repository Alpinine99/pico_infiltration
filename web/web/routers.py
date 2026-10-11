class PicoRouter:
    app_label = "main"

    def db_for_read(self, model, **hints):
        return "pico" if model._meta.app_label == self.app_label else None

    def db_for_write(self, model, **hints):
        return "pico" if model._meta.app_label == self.app_label else None

    def allow_relation(self, a, b, **hints):
        return None

    def allow_migrate(self, db, app_label, model_name=None, **hints):
        if app_label == self.app_label or db == "pico":
            return False
        return None

