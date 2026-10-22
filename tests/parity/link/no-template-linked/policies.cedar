@id("AccessVacation")
permit (principal in ?principal, action == Action::"view", resource == Photo::"VacationPhoto94.jpg");

@id("Static")
permit (principal, action == Action::"view", resource);

@id("Both")
permit (principal == ?principal, action, resource in ?resource);
