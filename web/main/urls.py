from django.urls import path

from . import views

# No trailing slashes, so POST /reboot/<id> matches exactly (APPEND_SLASH never redirects a POST).
urlpatterns = [
    path("", views.index, name="index"),
    path("api/readings", views.api_readings, name="api_readings"),
    path("api/log", views.api_log, name="api_log"),
    path("download", views.download, name="download"),
    path("reboot/<path:pico_id>", views.reboot, name="reboot"),
    path("sleep/<path:pico_id>", views.sleep_cmd, name="sleep"),
]
